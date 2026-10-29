// DualSense adaptive triggers on the PC, through SDL3. The behaviour is ps5/game/ps5_triggers.h's (Zaid
// Naeem, upstream): R2 is the throttle, a light spring that firms up towards the end of its travel; L2
// is the brake, firmer; R2 buzzes near the top of each gear with the throttle floored; and the triggers
// go slack whenever the car is not being driven (menus, loading, the pause menu). Not ported: the buzzes
// that ps5_triggers.h takes from the rumble level (wheelspin, ABS-like pulses, hits), because the PC has
// no rumble level to read: the guest's rumble goes from the SDK's input system to SDL and not through
// this file. R2 buzzes while the nitro burns instead (the nitro timer of src/vehicle_telemetry.h; the
// PS5's haptics use the same signal).
//
// How it reaches the pad: the SDK's SDL input driver has opened every gamepad (SDL refcounts an open
// gamepad), so this takes one more reference on each DualSense and sends the pad's output report with
// SDL_SendGamepadEffect: DS5EffectsState_t of SDL's hidapi PS5 driver (src/joystick/hidapi/
// SDL_hidapi_ps5.c, the layout in SDL's test/testcontroller.c): enable bits 0x04 (right trigger) and
// 0x08 (left trigger) and nothing else, so the rumble the game sends through SDL keeps working; the
// 11-byte trigger blocks at bytes 10 (R2) and 21 (L2). SDL wraps it for USB (report 0x02) and Bluetooth
// (report 0x31, with the CRC) and queues it for its HID writer thread. Effect bytes: mode 0x05 off, 0x21
// resistance per zone, 0x26 vibration per zone (a zone is a tenth of the travel, three bits of strength
// each; the same bytes as dualsensectl and Sony's own libraries write).
//
// Threads: one worker thread of this file does all the sending (SDL's gamepad calls are thread-safe).
// The pedals' positions are read where the game reads the pad (MclaPcTriggersFrame, once per update):
// the SDK's input system has no lock of its own, so the worker never touches it.
//
// An effect stays in the pad until the next report, so the triggers are released (mode 0x05) when the
// option goes off, when the game window is not in front, when the car is not driven, when the window is
// closed, QUIT GAME or RESTART GAME is chosen or the title ends (MclaPcTriggersShutdown), and once when a
// pad is first seen with the option on (a stale effect left by a game that was killed). A killed game
// cannot release them.

#include "generated/default/mcla_init.h"

#include "pc_triggers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "vehicle_telemetry.h"

// The names and defaults are the PS5 build's (ps5/game/ps5_triggers.h); the PS5's other half is in
// mcla.toml of the console. The PC differs in two ways: mcla_trigger_vibration buzzes R2 while the nitro
// burns (the PS5 buzzes on rumble peaks), and the options below it are the PC's own.
REXCVAR_DEFINE_BOOL(mcla_adaptive_triggers, false, "MCLA",
                    "DualSense adaptive triggers: throttle spring on R2, brake resistance on L2 (needs a "
                    "DualSense; other controllers are not touched)");
REXCVAR_DEFINE_INT32(mcla_trigger_throttle, 3, "MCLA",
                     "R2 resistance at the end of its travel, 1 to 8").range(1, 8);
REXCVAR_DEFINE_INT32(mcla_trigger_brake, 6, "MCLA",
                     "L2 resistance at the end of its travel, 1 to 8").range(1, 8);
REXCVAR_DEFINE_BOOL(mcla_trigger_vibration, true, "MCLA",
                    "R2 buzzes while the nitro burns with the throttle floored (the PS5 build also buzzes "
                    "on wheelspin, hard braking and hits; the PC has no rumble level to read)");
REXCVAR_DEFINE_INT32(mcla_trigger_vibration_strength, 3, "MCLA",
                     "Strength of those buzzes, 1 to 8").range(1, 8);
REXCVAR_DEFINE_BOOL(mcla_trigger_rev_limiter, true, "MCLA",
                    "R2 buzzes near the top of each gear with the throttle floored (after the game has "
                    "shifted up once in that gear: the shift speeds are learnt)");
REXCVAR_DEFINE_BOOL(mcla_trigger_release_unfocused, true, "MCLA",
                    "The triggers go slack while the game's window is not the one in front (Windows)");
REXCVAR_DEFINE_BOOL(mcla_trigger_log, false, "MCLA",
                    "Development: log every trigger effect sent (the bytes, SDL's answer, why), the state "
                    "changes and a status line a second");
REXCVAR_DEFINE_BOOL(mcla_trigger_fake_device, false, "MCLA",
                    "Development: act as if one more DualSense were connected, one that only logs what it "
                    "would be sent (for trying the logic without one)");

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kNoHudMs = 300;        // no HUD update for this long: not driving (as ps5_triggers.h)
constexpr int kMinIntervalMs = 40;   // between two sends to one trigger (SDL's writer spends 10 ms on each report)
constexpr int kTickMs = 20;
constexpr int kScanMs = 1000;
constexpr int kNitroHoldMs = 200;    // the nitro timer moves once per HUD update while it burns
constexpr int kPedalStaleMs = 250;
constexpr int kGears = 8;
constexpr uint16_t kSonyVendor = 0x054C;

// --- The effect bytes (11 per trigger) ---------------------------------------------------------------

struct Effect {
  uint8_t b[11] = {};
  bool operator==(const Effect& o) const { return std::memcmp(b, o.b, sizeof b) == 0; }
  bool operator!=(const Effect& o) const { return !(*this == o); }
};

Effect Off() {
  Effect e;
  e.b[0] = 0x05;
  return e;
}

// Zones start..9 get a strength each (0 = none), as mode 0x21 (resistance) or 0x26 (vibration; with a frequency).
Effect Zones(uint8_t mode, const int (&strength)[10], uint8_t frequency) {
  uint32_t force = 0;
  uint16_t active = 0;
  for (int i = 0; i < 10; ++i) {
    if (strength[i] > 0) {
      force |= uint32_t((std::clamp(strength[i], 1, 8) - 1) & 7) << (3 * i);
      active |= uint16_t(1u << i);
    }
  }
  Effect e;
  e.b[0] = mode;
  e.b[1] = uint8_t(active & 0xFF);
  e.b[2] = uint8_t(active >> 8);
  e.b[3] = uint8_t(force & 0xFF);
  e.b[4] = uint8_t((force >> 8) & 0xFF);
  e.b[5] = uint8_t((force >> 16) & 0xFF);
  e.b[6] = uint8_t((force >> 24) & 0xFF);
  e.b[9] = frequency;
  return e;
}

// Resistance rising from s0 at zone `start` to s1 at zone `end` (zones 0..9, strengths 1..8), then s1 to the end of
// the travel: Sony's "slope feedback", which ps5_triggers.h asks the console for.
Effect Slope(int start, int end, int s0, int s1) {
  int strength[10] = {};
  s0 = std::clamp(s0, 1, 8);
  s1 = std::clamp(s1, 1, 8);
  const float slope = end > start ? float(s1 - s0) / float(end - start) : 0.0f;
  for (int i = std::clamp(start, 0, 9); i < 10; ++i) {
    strength[i] = i > end ? s1 : int(std::lround(float(s0) + slope * float(i - start)));
  }
  return Zones(0x21, strength, 0);
}

// Vibration from zone `position` on, amplitude 1..8, frequency in Hz.
Effect Vibration(int position, int amplitude, int frequency) {
  int strength[10] = {};
  for (int i = std::clamp(position, 0, 9); i < 10; ++i) {
    strength[i] = std::clamp(amplitude, 1, 8);
  }
  return Zones(0x26, strength, uint8_t(std::clamp(frequency, 1, 255)));
}

// SDL's DS5EffectsState_t (src/joystick/hidapi/SDL_hidapi_ps5.c): the pad's output report without its header.
struct Ds5Effects {
  uint8_t enable_bits1;       // 0: 0x04 right trigger effect, 0x08 left trigger effect (0x01/0x02 are rumble)
  uint8_t enable_bits2;       // 1
  uint8_t rumble_right;       // 2
  uint8_t rumble_left;        // 3
  uint8_t headphone_volume;   // 4
  uint8_t speaker_volume;     // 5
  uint8_t microphone_volume;  // 6
  uint8_t audio_enable_bits;  // 7
  uint8_t mic_light_mode;     // 8
  uint8_t audio_mute_bits;    // 9
  uint8_t right_trigger[11];  // 10: R2
  uint8_t left_trigger[11];   // 21: L2
  uint8_t unknown1[6];        // 32
  uint8_t enable_bits3;       // 38
  uint8_t unknown2[2];        // 39
  uint8_t led_anim;           // 41
  uint8_t led_brightness;     // 42
  uint8_t pad_lights;         // 43
  uint8_t led_red;            // 44
  uint8_t led_green;          // 45
  uint8_t led_blue;           // 46
};
static_assert(sizeof(Ds5Effects) == 47, "SDL's DS5EffectsState_t is 47 bytes");

std::string Hex(const Effect& e) {
  char text[40];
  int n = 0;
  for (int i = 0; i < 11 && n < 36; ++i) {
    n += std::snprintf(text + n, sizeof text - size_t(n), i ? " %02x" : "%02x", e.b[i]);
  }
  return std::string(text, size_t(n));
}

// --- The pads ------------------------------------------------------------------------------------------------

enum Side { kL2 = 0, kR2 = 1 };
const char* const kSideName[2] = {"L2", "R2"};

struct Pad {
  SDL_JoystickID id = 0;
  SDL_Gamepad* gamepad = nullptr;  // our own reference to the one the game's input driver opened; null = the fake pad
  std::string name;
  bool fake = false;
  std::optional<Effect> current[2];  // what the pad was last sent (unset: never touched by this game)
  Clock::time_point last_send[2] = {};
  Clock::time_point retry_at = {};
  int failures = 0;
  bool gave_up = false;
};

struct Wanted {
  std::optional<Effect> effect[2];
  const char* why[2] = {"", ""};
};

class PcTriggers {
 public:
  void Run() {
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        if (stop_) break;
        cv_.wait_for(lock, std::chrono::milliseconds(sleep_ms_), [this] { return stop_; });
        if (stop_) break;
      }
      sleep_ms_ = Tick(Clock::now());
    }
    done_.store(true, std::memory_order_release);
  }

  // Stops the worker, releases both triggers of every pad and lets go of the pads. False if the worker did not stop.
  bool FinishAndRelease() {
    stop_requested_.store(true);
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    for (int i = 0; i < 100 && !done_.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!done_.load(std::memory_order_acquire)) {
      REXLOG_WARN("pc triggers: the worker did not stop within 500 ms; not releasing from here");
      return false;
    }
    bool sent = false;
    if (SdlUp()) {
      for (Pad& pad : pads_) {
        sent |= ReleasePad(pad, "shutdown");
      }
    }
    // SDL writes the report on a thread of its own, a few ms later (10 ms for each report ahead of it).
    if (sent) {
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    if (SdlUp()) {
      for (Pad& pad : pads_) {
        if (pad.gamepad) {
          SDL_CloseGamepad(pad.gamepad);
          pad.gamepad = nullptr;
        }
      }
    }
    pads_.clear();
    return true;
  }

  bool Engaged() const { return engaged_.load(std::memory_order_relaxed); }
  void SetPedals(uint8_t lt, uint8_t rt) {
    pedal_l2_.store(lt, std::memory_order_relaxed);
    pedal_r2_.store(rt, std::memory_order_relaxed);
    pedal_us_.store(NowUs(), std::memory_order_relaxed);
  }

 private:
  static int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
  }

  static bool SdlUp() { return (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) != 0; }

  static bool Logging() { return REXCVAR_GET(mcla_trigger_log); }

#ifdef _WIN32
  static bool WindowInFront() {
    HWND front = GetForegroundWindow();
    if (!front) {
      return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(front, &pid);
    return pid == GetCurrentProcessId();
  }
#else
  static bool WindowInFront() { return true; }
#endif

  // --- Pads ---

  void Scan(Clock::time_point now) {
    next_scan_ = now + std::chrono::milliseconds(kScanMs);
    // The fake pad (development).
    const bool fake_wanted = REXCVAR_GET(mcla_trigger_fake_device);
    const auto fake = std::find_if(pads_.begin(), pads_.end(), [](const Pad& p) { return p.fake; });
    if (fake_wanted && fake == pads_.end()) {
      Pad pad;
      pad.fake = true;
      pad.name = "fake DualSense (mcla_trigger_fake_device)";
      pads_.push_back(pad);
      REXLOG_INFO("pc triggers: using {}: effects are logged, not sent", pads_.back().name);
    } else if (!fake_wanted && fake != pads_.end()) {
      pads_.erase(fake);
    }
    if (!SdlUp()) {
      return;
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    std::vector<SDL_JoystickID> present;
    if (ids) {
      present.assign(ids, ids + count);
      SDL_free(ids);
    }
    for (auto it = pads_.begin(); it != pads_.end();) {
      const bool gone = it->gamepad && (std::find(present.begin(), present.end(), it->id) == present.end() ||
                                        !SDL_GamepadConnected(it->gamepad));
      if (gone) {
        REXLOG_INFO("pc triggers: {} (id {}) is gone", it->name, uint32_t(it->id));
        SDL_CloseGamepad(it->gamepad);
        it = pads_.erase(it);
      } else {
        ++it;
      }
    }
    for (SDL_JoystickID id : present) {
      if (std::any_of(pads_.begin(), pads_.end(), [id](const Pad& p) { return p.gamepad && p.id == id; }) ||
          std::find(skipped_.begin(), skipped_.end(), id) != skipped_.end()) {
        continue;
      }
      // Only a pad the game's input driver has opened: the pad the game reads.
      SDL_Gamepad* opened = SDL_GetGamepadFromID(id);
      if (!opened) {
        continue;
      }
      const char* name = SDL_GetGamepadName(opened);
      const SDL_GamepadType type = SDL_GetGamepadType(opened);
      const uint16_t vendor = SDL_GetGamepadVendor(opened);
      if (!IsDualSense(type, vendor)) {
        skipped_.push_back(id);
        REXLOG_INFO("pc triggers: {} (id {}, SDL type {}, vendor 0x{:04X}) is not a DualSense: not touched",
                    name ? name : "?", uint32_t(id), int(type), vendor);
        continue;
      }
      Pad pad;
      pad.id = id;
      pad.gamepad = SDL_OpenGamepad(id);  // another reference to the same open pad
      if (!pad.gamepad) {
        continue;
      }
      pad.name = name ? name : "DualSense";
      const SDL_JoystickConnectionState link = SDL_GetGamepadConnectionState(pad.gamepad);
      REXLOG_INFO("pc triggers: using {} (id {}, {}, product 0x{:04X})", pad.name, uint32_t(id),
                  link == SDL_JOYSTICK_CONNECTION_WIRELESS ? "Bluetooth"
                  : link == SDL_JOYSTICK_CONNECTION_WIRED  ? "USB"
                                                           : "link unknown",
                  SDL_GetGamepadProduct(pad.gamepad));
      pads_.push_back(pad);
    }
    if (!logged_scan_ && Logging()) {
      logged_scan_ = true;
      // What the classifier says for every kind of pad SDL knows: only the PS5 type from Sony passes.
      for (int t = 0; t < int(SDL_GAMEPAD_TYPE_COUNT); ++t) {
        REXLOG_INFO("pc triggers: classifier: SDL type {} vendor 0x054C -> {}; vendor 0x045E -> {}", t,
                    IsDualSense(SDL_GamepadType(t), kSonyVendor) ? "DualSense" : "not touched",
                    IsDualSense(SDL_GamepadType(t), 0x045E) ? "DualSense" : "not touched");
      }
    }
  }

  // A DualSense or DualSense Edge: SDL's PS5 type from Sony. Other PS5-type pads are left alone.
  static bool IsDualSense(SDL_GamepadType type, uint16_t vendor) {
    return type == SDL_GAMEPAD_TYPE_PS5 && vendor == kSonyVendor;
  }

  // --- Sending ---

  // One report with the given trigger blocks; the answer of SDL (true: queued for the pad).
  bool Send(Pad& pad, bool l2, bool r2, const Effect& l2_effect, const Effect& r2_effect, std::string& answer) {
    if (pad.fake) {
      answer = "fake (not sent)";
      return true;
    }
    Ds5Effects report = {};
    if (r2) {
      report.enable_bits1 |= 0x04;
      std::memcpy(report.right_trigger, r2_effect.b, sizeof report.right_trigger);
    }
    if (l2) {
      report.enable_bits1 |= 0x08;
      std::memcpy(report.left_trigger, l2_effect.b, sizeof report.left_trigger);
    }
    if (!SdlUp() || !SDL_SendGamepadEffect(pad.gamepad, &report, int(sizeof report))) {
      const char* error = SDL_GetError();
      answer = std::string("false: ") + (error && *error ? error : "no error text");
      return false;
    }
    answer = "true";
    return true;
  }

  // Sends what differs from what the pad holds. `release` ignores the rate limit.
  void Apply(Pad& pad, const Wanted& wanted, Clock::time_point now, bool release) {
    if (pad.gave_up || now < pad.retry_at) {
      return;
    }
    bool change[2] = {};
    Effect target[2];
    for (int t = 0; t < 2; ++t) {
      std::optional<Effect> want = wanted.effect[t];
      if (!want) {
        if (pad.current[t] && *pad.current[t] != Off()) {
          want = Off();  // nothing wanted any more: let go of what was sent
        } else {
          continue;
        }
      }
      if (pad.current[t] && *pad.current[t] == *want) {
        continue;
      }
      if (!release && pad.current[t] && now - pad.last_send[t] < std::chrono::milliseconds(kMinIntervalMs)) {
        continue;
      }
      change[t] = true;
      target[t] = *want;
    }
    if (!change[0] && !change[1]) {
      return;
    }
    std::string answer;
    const bool ok = Send(pad, change[kL2], change[kR2], target[kL2], target[kR2], answer);
    for (int t = 0; t < 2; ++t) {
      if (!change[t]) {
        continue;
      }
      if (Logging() || !ok) {
        REXLOG_INFO("pc triggers: {} -> {} [{}] ({}) sdl={} pad=\"{}\"", kSideName[t], Describe(target[t]),
                    Hex(target[t]), wanted.why[t][0] ? wanted.why[t] : (target[t] == Off() ? "released" : "?"),
                    answer, pad.name);
      }
      if (ok) {
        pad.current[t] = target[t];
        pad.last_send[t] = now;
      }
    }
    if (ok) {
      pad.failures = 0;
    } else {
      pad.retry_at = now + std::chrono::seconds(1);
      if (++pad.failures >= 8) {
        pad.gave_up = true;
        REXLOG_WARN("pc triggers: {} refused 8 reports in a row; giving up on it ({})", pad.name, answer);
      }
    }
  }

  static const char* Describe(const Effect& e) {
    switch (e.b[0]) {
      case 0x05: return "off";
      case 0x21: return "resistance";
      case 0x26: return "vibration";
      default: return "?";
    }
  }

  // Both triggers off, if the pad may hold an effect. True when a report was queued.
  bool ReleasePad(Pad& pad, const char* why) {
    if (pad.gave_up) {
      return false;
    }
    Wanted off;
    off.effect[kL2] = Off();
    off.effect[kR2] = Off();
    off.why[kL2] = off.why[kR2] = why;
    bool differs = false;
    for (int t = 0; t < 2; ++t) {
      differs |= pad.current[t] && *pad.current[t] != Off();
    }
    if (!differs) {
      return false;
    }
    Apply(pad, off, Clock::now(), true);
    return true;
  }

  // --- The decision ---

  // The effects for what the car is doing (empty when the triggers are to be let go).
  Wanted Decide(Clock::time_point now, std::string& state) {
    Wanted wanted;
    // As ps5_triggers.h: the HUD updates the car's telemetry every frame while a car is driven.
    const uint32_t hud = g_mcla_vehicle.hud_updates.load(std::memory_order_relaxed);
    if (hud != last_hud_) {
      last_hud_ = hud;
      last_hud_change_ = now;
      seen_hud_ = true;
    }
    const bool hud_alive = seen_hud_ && now - last_hud_change_ <= std::chrono::milliseconds(kNoHudMs);
    const bool front = !REXCVAR_GET(mcla_trigger_release_unfocused) || WindowInFront();
    const bool pause_menu = MclaPauseMenuUp();
    const bool frozen = MclaVehiclePaused();
    if (!front) {
      state = "window not in front";
    } else if (!hud_alive) {
      state = "not driving (menu, loading, no HUD)";
    } else if (pause_menu) {
      state = "pause menu";
    } else if (frozen) {
      state = "pause (the car's velocity is frozen)";
    } else {
      state.clear();
    }
    if (!state.empty()) {
      nitro_until_ = {};
      r2_limiter_ = false;
      return wanted;
    }

    const float speed = g_mcla_vehicle.speed.load(std::memory_order_relaxed);
    const int gear = g_mcla_vehicle.gear.load(std::memory_order_relaxed);
    const float nitro_time = g_mcla_vehicle.nitro_time.load(std::memory_order_relaxed);
    const bool pedals_fresh = NowUs() - pedal_us_.load(std::memory_order_relaxed) < int64_t(kPedalStaleMs) * 1000;
    const int l2 = pedals_fresh ? pedal_l2_.load(std::memory_order_relaxed) : 0;
    const int r2 = pedals_fresh ? pedal_r2_.load(std::memory_order_relaxed) : 0;

    // The nitro timer moves (once per HUD update) while it burns.
    if (nitro_time != last_nitro_time_ && nitro_time > 0.0f) {
      nitro_until_ = now + std::chrono::milliseconds(kNitroHoldMs);
    }
    last_nitro_time_ = nitro_time;
    const bool nitro = now < nitro_until_;

    // MCLA has no rev counter to read, but it shifts up at the top of each gear, so the speed of each upshift is
    // learnt and the speed against it stands in for the revs (as ps5_triggers.h).
    if (gear == last_gear_ + 1 && last_gear_ >= 1 && last_gear_ < kGears) {
      float& top = gear_top_[last_gear_];
      top = top > 0.0f ? top * 0.7f + speed * 0.3f : speed;
      if (Logging()) {
        REXLOG_INFO("pc triggers: shift {} -> {} at {:.1f} m/s; the top of gear {} is now taken as {:.1f} m/s",
                    last_gear_, gear, speed, last_gear_, top);
      }
    } else if (gear != last_gear_ && Logging()) {
      REXLOG_INFO("pc triggers: gear {} -> {} at {:.1f} m/s", last_gear_, gear, speed);
    }
    last_gear_ = gear;
    const bool can_limit = REXCVAR_GET(mcla_trigger_rev_limiter) && gear >= 1 && gear < kGears && gear_top_[gear] > 5.0f;
    // Entered above 96% of the shift speed with the throttle floored; left under 93% or off the throttle.
    if (can_limit && speed > gear_top_[gear] * 0.96f && r2 > 220) {
      r2_limiter_ = true;
    } else if (!can_limit || speed < gear_top_[gear] * 0.93f || r2 < 200) {
      r2_limiter_ = false;
    }

    const int strength = REXCVAR_GET(mcla_trigger_vibration_strength);
    if (REXCVAR_GET(mcla_trigger_vibration) && nitro && r2 > 200) {
      wanted.effect[kR2] = Vibration(3, strength, 30);
      wanted.why[kR2] = "nitro burning, throttle floored";
    } else if (r2_limiter_) {
      wanted.effect[kR2] = Vibration(4, std::max(1, strength - 1), 25);
      wanted.why[kR2] = "top of the gear, throttle floored";
    } else {
      wanted.effect[kR2] = Slope(1, 9, 1, REXCVAR_GET(mcla_trigger_throttle));
      wanted.why[kR2] = "throttle spring";
    }
    wanted.effect[kL2] = Slope(2, 8, 2, REXCVAR_GET(mcla_trigger_brake));
    wanted.why[kL2] = "brake resistance";
    status_speed_ = speed;
    status_gear_ = gear;
    status_nitro_ = nitro;
    status_r2_ = r2;
    status_l2_ = l2;
    return wanted;
  }

  // One pass; returns how long to sleep before the next.
  int Tick(Clock::time_point now) {
    if (stop_requested_.load()) {
      return kTickMs;
    }
    const bool option = REXCVAR_GET(mcla_adaptive_triggers);
    if (!option && pads_.empty() && !REXCVAR_GET(mcla_trigger_fake_device)) {
      engaged_.store(false, std::memory_order_relaxed);
      return 250;
    }
    if (now >= next_scan_) {
      Scan(now);
    }
    if (SdlUp() == false && !std::any_of(pads_.begin(), pads_.end(), [](const Pad& p) { return p.fake; })) {
      engaged_.store(false, std::memory_order_relaxed);
      return 250;
    }
    if (!option) {
      // Turned off: whatever was sent is let go of; nothing is sent to a pad this game never touched.
      if (was_option_) {
        was_option_ = false;
        if (Logging()) {
          REXLOG_INFO("pc triggers: option off: releasing");
        }
      }
      engaged_.store(false, std::memory_order_relaxed);
      state_ = "option off";
      for (Pad& pad : pads_) {
        ReleasePad(pad, "option off");
      }
      return pads_.empty() ? 250 : 100;
    }
    if (!was_option_) {
      was_option_ = true;
      if (Logging()) {
        REXLOG_INFO("pc triggers: option on (throttle {}, brake {}, buzzes {}, strength {}, rev limiter {}, "
                    "release when not in front {})",
                    REXCVAR_GET(mcla_trigger_throttle), REXCVAR_GET(mcla_trigger_brake),
                    REXCVAR_GET(mcla_trigger_vibration) ? "on" : "off", REXCVAR_GET(mcla_trigger_vibration_strength),
                    REXCVAR_GET(mcla_trigger_rev_limiter) ? "on" : "off",
                    REXCVAR_GET(mcla_trigger_release_unfocused) ? "yes" : "no");
      }
    }
    std::string state;
    Wanted wanted = Decide(now, state);
    const bool engaged = state.empty();
    engaged_.store(engaged, std::memory_order_relaxed);
    if (engaged != was_engaged_ || state != state_) {
      was_engaged_ = engaged;
      state_ = state;
      if (Logging()) {
        if (engaged) {
          REXLOG_INFO("pc triggers: state: driving ({:.1f} m/s, gear {})", status_speed_, status_gear_);
        } else {
          REXLOG_INFO("pc triggers: state: slack, {}", state);
        }
      }
    }
    if (!engaged) {
      // A pad seen for the first time is sent "off" once, whatever the state: it may hold an effect a killed game left.
      wanted.effect[kL2] = Off();
      wanted.effect[kR2] = Off();
      wanted.why[kL2] = wanted.why[kR2] = "slack";
    }
    for (Pad& pad : pads_) {
      Apply(pad, wanted, now, !engaged);
    }
    if (Logging() && engaged && now >= next_status_) {
      next_status_ = now + std::chrono::seconds(1);
      REXLOG_INFO("pc triggers: status: speed {:.1f} m/s gear {} r2 {} l2 {} nitro {} limiter {} hud {}", status_speed_,
                  status_gear_, status_r2_, status_l2_, status_nitro_ ? "burning" : "no", r2_limiter_ ? "yes" : "no",
                  g_mcla_vehicle.hud_updates.load(std::memory_order_relaxed));
    }
    return kTickMs;
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> done_{false};
  int sleep_ms_ = 250;

  std::vector<Pad> pads_;
  std::vector<SDL_JoystickID> skipped_;
  Clock::time_point next_scan_ = {};
  Clock::time_point next_status_ = {};
  bool logged_scan_ = false;

  std::atomic<bool> engaged_{false};
  std::atomic<uint8_t> pedal_l2_{0};
  std::atomic<uint8_t> pedal_r2_{0};
  std::atomic<int64_t> pedal_us_{0};

  bool was_option_ = false;
  bool was_engaged_ = false;
  std::string state_;
  uint32_t last_hud_ = 0;
  Clock::time_point last_hud_change_ = {};
  bool seen_hud_ = false;
  int last_gear_ = 0;
  float gear_top_[kGears] = {};
  float last_nitro_time_ = 0.0f;
  Clock::time_point nitro_until_ = {};
  bool r2_limiter_ = false;
  float status_speed_ = 0.0f;
  int status_gear_ = 0;
  bool status_nitro_ = false;
  int status_r2_ = 0;
  int status_l2_ = 0;
};

// Never destroyed: the process ends with _Exit, and a joinable std::thread must not be destroyed.
PcTriggers* g_triggers = nullptr;
std::thread* g_thread = nullptr;
std::atomic<bool> g_shut_down{false};

}  // namespace

void MclaPcTriggersStart() {
  if (g_triggers) {
    return;
  }
  g_triggers = new PcTriggers();
  g_thread = new std::thread([] { g_triggers->Run(); });
}

void MclaPcTriggersFrame() {
  if (!g_triggers || !g_triggers->Engaged()) {
    return;
  }
  auto* input = static_cast<rex::input::InputSystem*>(
      rex::Runtime::instance() ? rex::Runtime::instance()->input_system() : nullptr);
  if (!input) {
    return;
  }
  using rex::X_RESULT;  // in X_ERROR_SUCCESS
  rex::input::X_INPUT_STATE state = {};
  if (input->GetState(0, &state) == X_ERROR_SUCCESS) {
    g_triggers->SetPedals(state.gamepad.left_trigger, state.gamepad.right_trigger);
  }
}

void MclaPcTriggersShutdown() {
  if (!g_triggers || g_shut_down.exchange(true)) {
    return;
  }
  if (g_triggers->FinishAndRelease() && g_thread && g_thread->joinable()) {
    g_thread->join();
  }
  if (REXCVAR_GET(mcla_trigger_log)) {
    REXLOG_INFO("pc triggers: shut down; the triggers are released");
  }
}

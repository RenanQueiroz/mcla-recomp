// DualSense adaptive triggers on PS5.
//
// R2 is the throttle: a light spring that firms up towards the end of its
// travel. L2 is the brake: firmer, with resistance rising through the travel.
//
// MCLA rumbles the pad the whole time a car is driven (the engine), so the
// rumble level itself means nothing. What stands out from it does: a short
// rise well above its recent average while the throttle is floored
// (wheelspin, nitro, a scrape) gives R2 a short buzz; the same while braking
// hard gives L2 an ABS-like pulse; a sudden jump (a hit) kicks both. Each is
// short and gentle, so the triggers do not rattle all race long.
//
// The effect parameter is ScePadTriggerEffectParam, whose layout is public in
// the Steamworks SDK (isteamdualsense.h): a trigger mask, then one command per
// trigger (mode, then 48 bytes of mode data). Each trigger is sent on its own,
// only when its effect changes and at most every kMinIntervalMs. The resting
// resistance uses slope feedback; if the console refuses a mode, the next
// simpler one is used (multi-position feedback, then plain feedback).

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>

#include "ps5_haptics.h"

REXCVAR_DEFINE_BOOL(mcla_adaptive_triggers, false, "MCLA",
                    "DualSense adaptive triggers: throttle spring on R2, brake resistance on L2");
REXCVAR_DEFINE_INT32(mcla_trigger_throttle, 3, "MCLA",
                     "R2 resistance at the end of its travel, 1 to 8").range(1, 8);
REXCVAR_DEFINE_INT32(mcla_trigger_brake, 6, "MCLA",
                     "L2 resistance at the end of its travel, 1 to 8").range(1, 8);
REXCVAR_DEFINE_BOOL(mcla_trigger_vibration, true, "MCLA",
                    "Short trigger buzzes on wheelspin, hard braking and hits");
REXCVAR_DEFINE_INT32(mcla_trigger_vibration_strength, 3, "MCLA",
                     "Strength of those buzzes, 1 to 8").range(1, 8);

extern "C" {
int scePadSetTriggerEffect(int handle, const void* param);
}

class Ps5Triggers {
 public:
  ~Ps5Triggers() { Release(); }

  void Start(int pad_handle) {
    pad_handle_ = pad_handle;
    if (!REXCVAR_GET(mcla_adaptive_triggers)) return;
    MclaHapticsLog("PS5 triggers: on (throttle %d, brake %d, buzzes %s, strength %d)",
                   REXCVAR_GET(mcla_trigger_throttle), REXCVAR_GET(mcla_trigger_brake),
                   REXCVAR_GET(mcla_trigger_vibration) ? "on" : "off",
                   REXCVAR_GET(mcla_trigger_vibration_strength));
  }

  // Called with every controller read: the analogue trigger positions, 0..255.
  void Update(uint8_t l2, uint8_t r2) {
    if (pad_handle_ < 0 || !REXCVAR_GET(mcla_adaptive_triggers)) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_update_ < std::chrono::milliseconds(kMinIntervalMs)) return;
    last_update_ = now;

    const bool buzz = REXCVAR_GET(mcla_trigger_vibration);
    const int strength = REXCVAR_GET(mcla_trigger_vibration_strength);
    const int rumble = std::max<int>(g_mcla_haptics.large(), g_mcla_haptics.small());
    // The engine's steady rumble: an average over about two seconds.
    baseline_ += (rumble - baseline_) * 0.025f;
    const float rise = rumble - baseline_;

    // A hit: a sudden jump in the rumble kicks both triggers for a moment.
    if (buzz && rumble - last_rumble_ > kHitJump && now >= quiet_until_) {
      hit_until_ = now + std::chrono::milliseconds(kHitMs);
      quiet_until_ = hit_until_ + std::chrono::milliseconds(kCooldownMs);
    }
    last_rumble_ = rumble;
    if (now < hit_until_) {
      const Command kick = Vibration(0, std::min(8, strength + 2), 30);
      SendOne(0, kick);
      SendOne(1, kick);
      return;
    }

    // A rise well above the engine's rumble, with the pedal held: a short buzz,
    // at most kBuzzMs, then a rest before the next.
    const bool standing_out = rise > kRiseThreshold;
    const auto event = [&](bool pressed, std::chrono::steady_clock::time_point& until) {
      if (!buzz || !pressed || !standing_out) {
        if (now >= until) until = {};
        return false;
      }
      if (until == std::chrono::steady_clock::time_point{}) {
        if (now < quiet_until_) return false;
        until = now + std::chrono::milliseconds(kBuzzMs);
        quiet_until_ = until + std::chrono::milliseconds(kCooldownMs);
      }
      return now < until;
    };

    // R2: wheelspin, nitro or a scrape while the throttle is floored.
    SendOne(1, event(r2 > 200, r2_buzz_until_)
                   ? Vibration(3, strength, 30)
                   : Resistance(1, 9, 1, REXCVAR_GET(mcla_trigger_throttle)));
    // L2: an ABS-like pulse while braking hard.
    SendOne(0, event(l2 > 150, l2_buzz_until_)
                   ? Vibration(2, strength, 20)
                   : Resistance(2, 8, 2, REXCVAR_GET(mcla_trigger_brake)));
  }

  // Both triggers back to no effect.
  void Release() {
    if (pad_handle_ < 0 || !sent_any_) return;
    Param param{};
    param.trigger_mask = kMaskL2 | kMaskR2;
    scePadSetTriggerEffect(pad_handle_, &param);
    sent_any_ = false;
  }

 private:
  static constexpr int kModeFeedback = 1;
  static constexpr int kModeVibration = 3;
  static constexpr int kModeMultiplePositionFeedback = 4;
  static constexpr int kModeSlopeFeedback = 5;
  static constexpr uint8_t kMaskL2 = 0x01;
  static constexpr uint8_t kMaskR2 = 0x02;
  static constexpr int kMinIntervalMs = 40;
  static constexpr int kHitJump = 120;
  static constexpr int kHitMs = 120;
  static constexpr float kRiseThreshold = 70.0f;
  static constexpr int kBuzzMs = 400;
  static constexpr int kCooldownMs = 800;

  struct Command {
    int32_t mode = 0;
    uint8_t padding[4] = {};
    uint8_t data[48] = {};
    bool operator==(const Command& other) const {
      return mode == other.mode && std::memcmp(data, other.data, sizeof data) == 0;
    }
  };
  struct Param {
    uint8_t trigger_mask;
    uint8_t padding[7];
    Command command[2];  // L2, R2
  };
  static_assert(sizeof(Command) == 56 && sizeof(Param) == 120, "ScePadTriggerEffectParam layout");

  // Resistance rising from start_strength at position start to end_strength at
  // end (positions 0..9, strengths 1..8), in the best mode the console takes.
  Command Resistance(uint8_t start, uint8_t end, int start_strength, int end_strength) const {
    start_strength = std::clamp(start_strength, 1, 8);
    end_strength = std::clamp(end_strength, 1, 8);
    Command command;
    switch (resistance_mode_) {
      case kModeSlopeFeedback:
        command.mode = kModeSlopeFeedback;
        command.data[0] = start;
        command.data[1] = end;
        command.data[2] = static_cast<uint8_t>(start_strength);
        command.data[3] = static_cast<uint8_t>(end_strength);
        break;
      case kModeMultiplePositionFeedback:
        command.mode = kModeMultiplePositionFeedback;
        for (int position = 0; position < 10; ++position) {
          if (position < start) continue;
          const int step = std::min<int>(position, end) - start;
          command.data[position] = static_cast<uint8_t>(
              start_strength + (end_strength - start_strength) * step / std::max(1, end - start));
        }
        break;
      default:
        command.mode = kModeFeedback;
        command.data[0] = start;
        command.data[1] = static_cast<uint8_t>((start_strength + end_strength) / 2);
        break;
    }
    return command;
  }

  // Vibration from position on (0..9), amplitude 1..8, frequency in Hz.
  static Command Vibration(uint8_t position, int amplitude, uint8_t frequency) {
    Command command;
    command.mode = kModeVibration;
    command.data[0] = position;
    command.data[1] = static_cast<uint8_t>(std::clamp(amplitude, 1, 8));
    command.data[2] = frequency;
    return command;
  }

  // One trigger (0 = L2, 1 = R2), sent only when its effect changes.
  void SendOne(int trigger, const Command& command) {
    if (sent_[trigger] && command == last_[trigger]) return;
    Param param{};
    param.trigger_mask = trigger == 0 ? kMaskL2 : kMaskR2;
    param.command[trigger] = command;
    const int result = scePadSetTriggerEffect(pad_handle_, &param);
    if (result != 0) {
      if (failures_logged_ < 8) {
        ++failures_logged_;
        MclaHapticsLog("PS5 triggers: %s mode %d [%u %u %u %u] refused: 0x%08X",
                       trigger == 0 ? "L2" : "R2", command.mode, command.data[0], command.data[1],
                       command.data[2], command.data[3], static_cast<uint32_t>(result));
      }
      // The resting resistance falls back to a simpler mode.
      if (command.mode == resistance_mode_ && resistance_mode_ != kModeFeedback) {
        resistance_mode_ = resistance_mode_ == kModeSlopeFeedback ? kModeMultiplePositionFeedback
                                                                  : kModeFeedback;
        MclaHapticsLog("PS5 triggers: resistance now uses mode %d", resistance_mode_);
      }
      sent_[trigger] = false;
      return;
    }
    if (!logged_ok_[command.mode & 7]) {
      logged_ok_[command.mode & 7] = true;
      MclaHapticsLog("PS5 triggers: %s mode %d accepted", trigger == 0 ? "L2" : "R2", command.mode);
    }
    last_[trigger] = command;
    sent_[trigger] = true;
    sent_any_ = true;
  }

  int pad_handle_ = -1;
  int resistance_mode_ = kModeSlopeFeedback;
  float baseline_ = 0.0f;
  int last_rumble_ = 0;
  std::chrono::steady_clock::time_point last_update_{};
  std::chrono::steady_clock::time_point hit_until_{};
  std::chrono::steady_clock::time_point quiet_until_{};
  std::chrono::steady_clock::time_point r2_buzz_until_{};
  std::chrono::steady_clock::time_point l2_buzz_until_{};
  Command last_[2];
  bool sent_[2] = {false, false};
  bool sent_any_ = false;
  bool logged_ok_[8] = {};
  int failures_logged_ = 0;
};

inline Ps5Triggers g_mcla_triggers;

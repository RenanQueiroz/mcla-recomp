// Controller vibration on or off (fork issue #31): mcla_vibration, on by default, as on the console. The game
// has no vibration setting of its own (on the console players turned it off in the system's settings).
//
// Where the game's rumble goes: the game sets it in one place, sub_8275CB88 (called from the per-pad update,
// sub_82614D30, whenever a pad's rumble changed; the call itself is `bl XamInputSetState` at 0x8275CC34). The
// SDK takes it from there to InputSystem::SetState, which hands it to every device of the user: the SDL driver
// (SDL_RumbleGamepad, armed for SDL's longest time since XInput's rumble holds until changed) or the XInput
// driver (XInputSetState), whichever input_backend names, and the dev pad's synthetic device. The game only
// asks when the rumble changes, so a motor that is running keeps running until the game says stop.
//
// The switch sits in front of that call (config/vibration.toml, mcla_vibration_set), above every driver, so
// it needs no SDK patch and does the same on both:
//  - on: the call goes through as the game made it;
//  - off: the call is not made (the game is told it worked), except one that stops a motor the drivers were
//    last told to run: that one goes through with the motors at 0;
//  - what the game last asked for is kept either way. MclaVibrationFrame, every game update, makes the
//    drivers' state follow the switch: turned off while a motor runs, it is stopped there and then; turned on
//    while the game wants a rumble, that rumble is sent (so a long rumble the game set while the switch was off
//    is not lost to it).
// Nothing here touches mcla_adaptive_triggers (src/pc_triggers.cpp): the DualSense's triggers are another
// report, with its own option; a rumble report from SDL leaves a trigger effect as it is.
//
// Development: mcla_vibration_log logs every rumble call of the game and what each does, every driver call the
// dev pad's synthetic device gets and every send of MclaVibrationFrame; mcla_vibration_probe = "user,left,right"
// (for example 0,32768,49152) makes the game's own function (sub_8275CB88) run with those values on the game
// loop's thread, as if the game had set them, so that a script can check the switch without a car crashing
// (launcher/dev/scripts/vibration.pad, launcher/dev/vibration_check.py).

#include "generated/default/mcla_init.h"

#include "vibration.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

REXCVAR_DEFINE_BOOL(mcla_vibration, true, "MCLA",
                    "Controller vibration: the game's rumble goes to the controllers. Off: none of it does, "
                    "and a motor that is running stops (changes during play; separate from "
                    "mcla_adaptive_triggers)");
REXCVAR_DEFINE_BOOL(mcla_vibration_log, false, "MCLA",
                    "Development: log the game's rumble calls and what mcla_vibration does with them, and "
                    "the calls the dev pad's synthetic device gets");
REXCVAR_DEFINE_STRING(mcla_vibration_probe, "", "MCLA",
                      "Development (needs a dev pad script or pipe; set while running): \"user,left,right\" "
                      "makes the game's rumble function run with those values, as if the game had set them");

namespace {

constexpr uint32_t kUsers = 4;
// sub_8275CB88(user, &{left, right}): the game's one caller of XamInputSetState.
constexpr uint32_t kRumbleFn = 0x8275CB88;
// The game's malloc (src/settings_menu.cpp has the same), for the probe's four bytes.
constexpr uint32_t kGuestMallocFn = 0x82130528;

// Both motors in one word, left in the high half (the order they have in guest memory).
constexpr uint32_t Pack(uint32_t left, uint32_t right) {
  return ((left & 0xFFFFu) << 16) | (right & 0xFFFFu);
}

struct Slot {
  std::atomic<uint32_t> wanted{0};  // what the game last asked for
  std::atomic<uint32_t> driven{0};  // what the drivers were last told
};
Slot g_slots[kUsers];

std::atomic<uint64_t> g_guest_calls{0};
std::atomic<uint64_t> g_driver_calls{0};

bool Logging() {
  return REXCVAR_GET(mcla_vibration_log);
}

uint32_t Read32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

rex::input::InputSystem* Input() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
}

// The drivers told to set the motors, outside the game's own call.
void Send(uint32_t user, uint32_t values, const char* why) {
  auto* input = Input();
  if (!input) {
    return;  // not yet: tried again at the next update
  }
  g_slots[user].driven.store(values, std::memory_order_relaxed);
  rex::input::X_INPUT_VIBRATION vibration;
  vibration.left_motor_speed = static_cast<uint16_t>(values >> 16);
  vibration.right_motor_speed = static_cast<uint16_t>(values);
  input->SetState(user, &vibration);
  if (Logging()) {
    REXLOG_INFO("vibration: sent user={} left={} right={} ({}; no call from the game)", user,
                values >> 16, values & 0xFFFFu, why);
  }
}

// --- The probe (development) ---------------------------------------------------

struct Probe {
  uint32_t user = 0;
  uint32_t left = 0;
  uint32_t right = 0;
};
std::mutex g_probe_mutex;
Probe g_probe;
std::atomic<bool> g_probe_pending{false};

// From the thread that sets the option (the dev pad's script): only keeps the values; the game's function is
// run by the game loop, in MclaVibrationFrame.
void OnProbeSet(std::string_view, std::string_view value) {
  unsigned user = 0, left = 0, right = 0;
  const std::string text(value);
  if (text.empty()) {
    return;
  }
  if (std::sscanf(text.c_str(), "%u,%u,%u", &user, &left, &right) != 3 || user >= kUsers ||
      left > 0xFFFFu || right > 0xFFFFu) {
    REXLOG_WARN("vibration: mcla_vibration_probe '{}' is not user,left,right (user 0 to 3, motors 0 to 65535)",
                text);
    return;
  }
  {
    std::lock_guard lock(g_probe_mutex);
    g_probe = {user, left, right};
  }
  g_probe_pending.store(true, std::memory_order_release);
}

void RunProbe() {
  static std::once_flag registered;
  std::call_once(registered, [] {
    rex::cvar::RegisterChangeCallback("mcla_vibration_probe", OnProbeSet);
  });
  if (!g_probe_pending.exchange(false, std::memory_order_acquire)) {
    return;
  }
  Probe probe;
  {
    std::lock_guard lock(g_probe_mutex);
    probe = g_probe;
  }
  auto* runtime = rex::Runtime::instance();
  if (!runtime) {
    return;
  }
  PPCFunc* rumble = runtime->function_dispatcher()->GetFunction(kRumbleFn);
  PPCFunc* guest_malloc = runtime->function_dispatcher()->GetFunction(kGuestMallocFn);
  if (!rumble || !guest_malloc) {
    REXLOG_WARN("vibration: the probe needs the game's functions 0x{:08X} and 0x{:08X}", kRumbleFn,
                kGuestMallocFn);
    return;
  }
  static uint32_t buffer = 0;  // four bytes of the game's heap, kept for the next probe
  if (!buffer) {
    buffer = rex::ppc::GuestToHostFunction<uint32_t>(guest_malloc, 16u);
  }
  if (!buffer) {
    return;
  }
  uint8_t* p = runtime->virtual_membase() + buffer;
  p[0] = uint8_t(probe.left >> 8);
  p[1] = uint8_t(probe.left);
  p[2] = uint8_t(probe.right >> 8);
  p[3] = uint8_t(probe.right);
  if (Logging()) {
    REXLOG_INFO("vibration: probe: the game's function runs with user={} left={} right={}", probe.user,
                probe.left, probe.right);
  }
  const uint32_t result = rex::ppc::GuestToHostFunction<uint32_t>(rumble, probe.user, buffer);
  if (Logging()) {
    REXLOG_INFO("vibration: probe: the game's function returned {}", result);
  }
}

}  // namespace

// 0x8275CC34 in sub_8275CB88: `bl XamInputSetState`, r3 = the user, r5 = the vibration on the stack (two
// 16-bit motor speeds, left first, big-endian), r4 = 0. true skips the call (to 0x8275CC38) with r3 = 0, which
// is what the game takes as done.
bool mcla_vibration_set(PPCRegister& r3, PPCRegister& r5) {
  auto* runtime = rex::Runtime::instance();
  uint8_t* base = runtime ? runtime->virtual_membase() : nullptr;
  if (!base) {
    return false;
  }
  uint8_t* vibration = base + r5.u32;
  const uint32_t user = r3.u32;
  const uint32_t values = Read32(vibration);
  Slot* slot = user < kUsers ? &g_slots[user] : nullptr;
  const uint64_t n = g_guest_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (slot) {
    slot->wanted.store(values, std::memory_order_relaxed);
  }
  if (REXCVAR_GET(mcla_vibration)) {
    if (slot) {
      slot->driven.store(values, std::memory_order_relaxed);
    }
    if (Logging()) {
      REXLOG_INFO("vibration: game #{} user={} left={} right={}: to the drivers (on)", n, user,
                  values >> 16, values & 0xFFFFu);
    }
    return false;
  }
  if (slot && slot->driven.load(std::memory_order_relaxed) != 0) {
    // A motor may be running: this call is the one that stops it.
    std::memset(vibration, 0, 4);
    slot->driven.store(0, std::memory_order_relaxed);
    if (Logging()) {
      REXLOG_INFO("vibration: game #{} user={} left={} right={}: sent as a stop, motors 0 (off)", n, user,
                  values >> 16, values & 0xFFFFu);
    }
    return false;
  }
  r3.u64 = 0;
  if (Logging()) {
    REXLOG_INFO("vibration: game #{} user={} left={} right={}: held back, no driver call (off)", n, user,
                values >> 16, values & 0xFFFFu);
  }
  return true;
}

void MclaVibrationFrame() {
  // The update timer is also called from other threads; the first caller is the game loop (as in src/dev_pad.cpp).
  static const std::thread::id owner = std::this_thread::get_id();
  if (std::this_thread::get_id() != owner) {
    return;
  }
  if (Logging()) {
    RunProbe();
  }
  const bool on = REXCVAR_GET(mcla_vibration);
  for (uint32_t user = 0; user < kUsers; ++user) {
    Slot& slot = g_slots[user];
    const uint32_t want = on ? slot.wanted.load(std::memory_order_relaxed) : 0;
    if (want != slot.driven.load(std::memory_order_relaxed)) {
      Send(user, want, on ? "switched on" : "switched off");
    }
  }
}

void MclaVibrationDriverCall(uint32_t left, uint32_t right) {
  const uint64_t n = g_driver_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (Logging()) {
    REXLOG_INFO("vibration: driver call #{} left={} right={}", n, left, right);
  }
}

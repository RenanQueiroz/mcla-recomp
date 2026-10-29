// Title screen input: a development trace, and two options that work around Start needing many
// presses on the title screen and the attract video (fork issue; owner's report 2026-10-29).
//
// What the image does with a controller (read in the recompiled code, not changed by the fork):
// once per game update sub_82614D30 reads XInput into a 64-byte pad record (pad n at
// 0x82905388 + 64 n): the word at +12 is the previous update's button mask, the word at +8 the
// new one (LB 4, RB 8, Y 16, B 32, A 64, X 128, Back 256, left stick 512, right stick 1024,
// Start 2048, up 4096, right 8192, down 16384, left 32768). A press is "just pressed" for ONE
// update: (+8 xor +12) and +8. The menus' UI input object (sub_82223CD8) turns it into a key event
// (Start = key 17) once per UI manager update and hands it to sub_82209C38, which offers it to the
// modal scenes, the main form and its current child form, and last to the "UserInterface" object;
// whichever takes it, the title's own decision is made there. The title is the statechart state
// StartScreen, which waits for the event UI.startscreenexit (EVENT_START_SCREEN_EXIT, handled in
// sub_821FAD78).
//
// mcla_pad_probe (development): logs, with the time and the update number of the pad, every
// button edge of the pad records, and for 1.5 s after each Start press the decisions of that key
// path: which of the three ways took the key (the object and its vtable, the function), the main
// form's child, events posted, EVENT_START_SCREEN_EXIT, the movie player's start and stop, and
// how many times the movies ticked, the input mapper ran and the scripts asked "just pressed" in
// each update. Once a second it prints the counts. Every line starts with "TS ". The vtable
// addresses are the class (the report script reads them from a table made from the image's RTTI).
//
// mcla_title_fps (off by default, NOT measured as a fix yet): while the title screen, the
// start-up and the attract video run (the lighting set-up that comes with the city has not run
// yet: the guard bit at 0x82836A28 is 0), the frame-rate cap is this many frames a second, except
// for a short time after a Start press (so that the load after it runs at the player's cap).
// Everything measured so far says Start is accepted at the first press at 30 FPS and by chance
// at 60 FPS and above.
//
// mcla_title_pad_hold_ms (off by default, NOT measured as a fix yet): while the same is true,
// a press of Start or A stays "just pressed" in the pad record for this many milliseconds, not
// only in the update of the press, whatever the game's readers do with it.

#include "generated/default/mcla_init.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(mcla_pad_probe, false, "MCLA",
                    "Development: log the pad's button edges and, around each Start press, the "
                    "decisions of the key path (lines starting with TS), for the title screen "
                    "input check");
REXCVAR_DEFINE_INT32(mcla_title_fps, 0, "MCLA",
                     "On the title screen and the attract video the frame-rate cap is this many "
                     "frames a second (0 = the player's cap), except just after a Start press")
    .range(0, 120);
REXCVAR_DEFINE_INT32(mcla_title_pad_hold_ms, 0, "MCLA",
                     "On the title screen and the attract video a press of Start or A stays "
                     "\"just pressed\" in the pad record for this many milliseconds (0 = only in "
                     "the update of the press, as the game does)")
    .range(0, 500);

namespace {
constexpr uint32_t kPadBase = 0x82905388;
constexpr uint32_t kPadStride = 64;
constexpr uint32_t kPadCount = 4;
constexpr uint32_t kStartBit = 2048;
constexpr uint32_t kHoldMask = 2048 | 64;  // Start, A
// Bit 0 of a function-local static's guard: 0 until the city's lighting set-up has run.
constexpr uint32_t kCityGuard = 0x82836A28;
// The dev pad's "game state" word and the "city pending" word (src/dev_pad.cpp), logged to see
// the title phase end.
constexpr uint32_t kGameState = 0x8288D000;
constexpr uint32_t kCityPending = 0x8286CF54;
// The UI manager (G1), the UI logic (G2): pointers in the image's data.
constexpr uint32_t kUiManagerPtr = 0x8286D804;
constexpr uint32_t kUiLogicPtr = 0x8286D8D4;
// How long after a Start press the key path is logged, and how long the title cap stays off.
constexpr uint64_t kWindowUs = 1'500'000;
constexpr uint64_t kCapReleaseUs = 400'000;

uint64_t NowUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

uint32_t ReadGuestU32(uint32_t address) {
  if (address < 0x10000) {
    return 0;
  }
  const uint8_t* p = rex::Runtime::instance()->virtual_membase() + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint32_t ReadGuestU8(uint32_t address) {
  if (address < 0x10000) {
    return 0;
  }
  return *(rex::Runtime::instance()->virtual_membase() + address);
}

void WriteGuestU32(uint32_t address, uint32_t value) {
  if (address < 0x10000) {
    return;
  }
  uint8_t* p = rex::Runtime::instance()->virtual_membase() + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

bool InTitlePhase() {
  return (ReadGuestU32(kCityGuard) & 1) == 0;
}

// The object's vtable (its first word), 0 for none.
uint32_t Vtable(uint32_t object) {
  return object ? ReadGuestU32(object) : 0;
}

bool Probe() {
  return REXCVAR_GET(mcla_pad_probe);
}

std::atomic<uint64_t> g_window_until_us{0};
std::atomic<uint64_t> g_last_start_us{0};  // the last Start press, for the title cap
std::atomic<uint64_t> g_update{0};         // pad 0's update number

// A line is logged only inside the window after a Start press.
bool Window() {
  return Probe() && NowUs() < g_window_until_us.load(std::memory_order_relaxed);
}

// Counts of what ran, per second.
std::atomic<uint64_t> g_ui_updates{0}, g_mapper_calls{0}, g_movie_adv{0}, g_movie_ticks{0},
    g_native_entry{0}, g_native_start{0}, g_native_pass{0}, g_events{0}, g_dispatch{0};

// Only the game's thread (the one that runs the updates) touches these.
std::array<uint64_t, kPadCount> g_updates{};
std::array<uint32_t, kPadCount> g_hold_bits{};
std::array<uint64_t, kPadCount> g_hold_until_us{};

std::atomic<uint64_t> g_next_report_us{0};
uint64_t g_report_updates = 0;

void MaybeReport(uint64_t now) {
  uint64_t due = g_next_report_us.load(std::memory_order_relaxed);
  if (!due) {
    g_next_report_us.store(now + 1'000'000, std::memory_order_relaxed);
    return;
  }
  if (now < due) {
    return;
  }
  g_next_report_us.store(now + 1'000'000, std::memory_order_relaxed);
  REXLOG_INFO(
      "TS 1s t={} updates={} (pad0 total {}) ui_updates={} mapper={} movie_adv={} movie_ticks={} "
      "native_calls={} native_start={} native_pass={} events={} dispatch={} title_phase={} (game "
      "state {}, city pending {})",
      now, g_updates[0] - g_report_updates, g_updates[0], g_ui_updates.exchange(0),
      g_mapper_calls.exchange(0), g_movie_adv.exchange(0), g_movie_ticks.exchange(0),
      g_native_entry.exchange(0), g_native_start.exchange(0), g_native_pass.exchange(0),
      g_events.exchange(0), g_dispatch.exchange(0), InTitlePhase(), ReadGuestU32(kGameState),
      ReadGuestU32(kCityPending));
  g_report_updates = g_updates[0];
}

// What the key path looks at when Start is pressed: the modal ring, the main form and its child.
void LogState(uint32_t pad, uint64_t update) {
  const uint32_t g1 = ReadGuestU32(kUiManagerPtr);
  const uint32_t g2 = ReadGuestU32(kUiLogicPtr);
  const uint32_t impl = g1 ? ReadGuestU32(g1 + 52) : 0;
  const uint32_t user_interface = g1 ? ReadGuestU32(g1 + 40) : 0;
  const uint32_t ring = impl ? impl + 0x40724 : 0;
  const uint32_t main_form = g2 ? g2 + 0x13F80 : 0;
  const uint32_t child_a = main_form ? ReadGuestU32(main_form + 216) : 0;
  const uint32_t child_a_obj = child_a ? ReadGuestU32(child_a) : 0;
  const uint32_t array = main_form ? ReadGuestU32(main_form + 68) : 0;
  const uint32_t child_b = array ? ReadGuestU32(array + 436) : 0;
  const uint32_t r0 = ring ? ReadGuestU32(ring) : 0;
  const uint32_t r1 = ring ? ReadGuestU32(ring + 4) : 0;
  const uint32_t r2 = ring ? ReadGuestU32(ring + 8) : 0;
  REXLOG_INFO(
      "TS state pad{} update={} modal_count={} ring=[{:08X} (vt {:08X}), {:08X} (vt {:08X}), {:08X} "
      "(vt {:08X})] head={} tail={} flag13F58={} main_form (vt {:08X}) child_a={:08X} (vt {:08X}) "
      "child_b={:08X} (vt {:08X}) user_interface={:08X} (vt {:08X}) game_state={} city_pending={}",
      pad, update, impl ? ReadGuestU32(impl + 0x40738) : 0, r0, Vtable(r0), r1, Vtable(r1), r2,
      Vtable(r2), ring ? ReadGuestU32(ring + 12) : 0, ring ? ReadGuestU32(ring + 16) : 0,
      g2 ? ReadGuestU8(g2 + 0x13F58) : 0, Vtable(main_form), child_a, child_a_obj, child_b,
      Vtable(child_b), user_interface, Vtable(user_interface), ReadGuestU32(kGameState),
      ReadGuestU32(kCityPending));
}
}  // namespace

// The frame-rate cap for the title screen (called from the frame limiter's cap, about every
// 250 ms): `cap` is the player's cap (0 = uncapped, negative = original timing).
int32_t MclaTitleFpsCap(int32_t cap) {
  const int32_t title = REXCVAR_GET(mcla_title_fps);
  if (title <= 0 || cap < 0 || (cap > 0 && cap <= title) || !InTitlePhase()) {
    return cap;
  }
  const uint64_t last_start = g_last_start_us.load(std::memory_order_relaxed);
  if (last_start && NowUs() - last_start < kCapReleaseUs) {
    return cap;  // a press was just made: the load after it runs at the player's cap
  }
  return title;
}

// 0x82615048 in sub_82614D30: the pad record (r31) has its new button mask (+8) and the previous
// one (+12); runs once per update for every pad that is connected.
void mcla_pad_probe_update(PPCRegister& r31) {
  const uint32_t pad = (r31.u32 - kPadBase) / kPadStride;
  if (r31.u32 < kPadBase || pad >= kPadCount) {
    return;
  }
  const bool probe = Probe();
  const int32_t hold_ms = REXCVAR_GET(mcla_title_pad_hold_ms);
  if (!probe && hold_ms <= 0 && REXCVAR_GET(mcla_title_fps) <= 0) {
    return;
  }
  const uint64_t now = NowUs();
  const uint64_t update = ++g_updates[pad];
  uint32_t cur = ReadGuestU32(r31.u32 + 8);
  uint32_t last = ReadGuestU32(r31.u32 + 12);
  const uint32_t pressed = cur & ~last;
  const uint32_t released = last & ~cur;
  if (pad == 0) {
    g_update.store(update, std::memory_order_relaxed);
  }
  if (pressed & kStartBit) {
    g_last_start_us.store(now, std::memory_order_relaxed);
    if (probe) {
      g_window_until_us.store(now + kWindowUs, std::memory_order_relaxed);
    }
  }
  if (probe && (pressed || released)) {
    REXLOG_INFO("TS pad{} edge t={} update={} down={:#06x} up={:#06x} cur={:#06x}", pad, now,
                update, pressed, released, cur);
    if (pressed & kStartBit) {
      LogState(pad, update);
    }
  }
  if (hold_ms > 0 && InTitlePhase()) {
    // Keep a press of Start or A "just pressed" for hold_ms: down in the mask, not in the
    // previous one. The next update's "previous" is copied from this mask, so nothing changes
    // after the hold ends.
    if (pressed & kHoldMask) {
      g_hold_bits[pad] |= pressed & kHoldMask;
      g_hold_until_us[pad] = now + uint64_t(hold_ms) * 1000;
    }
    if (g_hold_bits[pad] && now < g_hold_until_us[pad]) {
      const uint32_t bits = g_hold_bits[pad];
      const uint32_t new_cur = cur | bits;
      const uint32_t new_last = last & ~bits;
      if (new_cur != cur) {
        WriteGuestU32(r31.u32 + 8, new_cur);
      }
      if (new_last != last) {
        WriteGuestU32(r31.u32 + 12, new_last);
      }
    } else {
      g_hold_bits[pad] = 0;
    }
  } else {
    g_hold_bits[pad] = 0;
  }
  if (probe && pad == 0) {
    MaybeReport(now);
  }
}


// 0x822240E4 in sub_82223CD8: after a menu key down was sent to the scene stack (sub_82209C38).
// r30 = whether something took it, r31 = the input (7 = Start), r4 = its key code or -1 (and
// nothing was sent).
void mcla_pad_probe_key(PPCRegister& r30, PPCRegister& r31, PPCRegister& r4) {
  if (!Probe()) {
    return;
  }
  REXLOG_INFO("TS menu key down input={} key={} taken={} t={} update={}", r31.u32, r4.s32,
              r30.u32 & 0xFF, NowUs(), g_update.load(std::memory_order_relaxed));
}

// ---- The key path (all of these log only in the window after a Start press) ----

// 0x82209C38: sub_82209C38(this = the UI logic, r4 = key code, r5 = whether an earlier key was
// taken) is entered for every key down and repeat.
void mcla_tsp_dispatch(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  if (!Probe()) {
    return;
  }
  g_dispatch.fetch_add(1, std::memory_order_relaxed);
  if (Window()) {
    REXLOG_INFO("TS dispatch key={} prev={} this={:08X} update={} t={}", r4.s32, r5.u32 & 0xFF,
                r3.u32, g_update.load(std::memory_order_relaxed), NowUs());
  }
}

// 0x82209CE0: way A, a modal scene (the ring): r3 = the scene, r8 = its vtable, r4 = the key;
// the function called is vtable[276].
void mcla_tsp_dispatch_a(PPCRegister& r3, PPCRegister& r4, PPCRegister& r8) {
  if (Window()) {
    REXLOG_INFO("TS way A: modal scene {:08X} vtable {:08X} handler {:08X} key={}", r3.u32, r8.u32,
                ReadGuestU32(r8.u32 + 276), r4.s32);
  }
}

// 0x82209CEC, 0x82209D40, 0x82209D6C: after the call of way A, B1 (the main form), B2 (the
// "UserInterface" object): r3 = what it answered.
void mcla_tsp_dispatch_a_res(PPCRegister& r3) {
  if (Window()) {
    REXLOG_INFO("TS way A answered {}", r3.u32 & 0xFF);
  }
}

// 0x82209D34: way B1, the main form at G2 + 0x13F80: r3 = it, r11 = its vtable; vtable[32].
void mcla_tsp_dispatch_b1(PPCRegister& r3, PPCRegister& r4, PPCRegister& r11) {
  if (Window()) {
    REXLOG_INFO("TS way B1: main form {:08X} vtable {:08X} handler {:08X} key={}", r3.u32, r11.u32,
                ReadGuestU32(r11.u32 + 32), r4.s32);
  }
}

void mcla_tsp_dispatch_b1_res(PPCRegister& r3) {
  if (Window()) {
    REXLOG_INFO("TS way B1 answered {}", r3.u32 & 0xFF);
  }
}

// 0x82209D60: way B2, the "UserInterface" object at G1 + 40: r3 = it, r11 = its vtable.
void mcla_tsp_dispatch_b2(PPCRegister& r3, PPCRegister& r4, PPCRegister& r11) {
  if (Window()) {
    REXLOG_INFO("TS way B2: user interface {:08X} vtable {:08X} handler {:08X} key={}", r3.u32,
                r11.u32, ReadGuestU32(r11.u32 + 276), r4.s32);
  }
}

void mcla_tsp_dispatch_b2_res(PPCRegister& r3) {
  if (Window()) {
    REXLOG_INFO("TS way B2 answered {}", r3.u32 & 0xFF);
  }
}

// 0x82643B0C in sub_826439E0 (the main form's key handler): the child form is offered the key:
// r3 = the child, r4 = the key, r11 = its vtable; vtable[276].
void mcla_tsp_child(PPCRegister& r3, PPCRegister& r4, PPCRegister& r11) {
  if (Window()) {
    REXLOG_INFO("TS main form -> child {:08X} vtable {:08X} handler {:08X} key={}", r3.u32,
                r11.u32, ReadGuestU32(r11.u32 + 276), r4.s32);
  }
}

// 0x82643B18: after the child answered (r3).
void mcla_tsp_child_res(PPCRegister& r3) {
  if (Window()) {
    REXLOG_INFO("TS child answered {}", r3.u32 & 0xFF);
  }
}

// 0x82631E20: the form key handler (sub_82631E20), r3 = the form, r4 = the key.
void mcla_tsp_form_key(PPCRegister& r3, PPCRegister& r4) {
  if (Window()) {
    REXLOG_INFO("TS form key: form {:08X} vtable {:08X} key={}", r3.u32, Vtable(r3.u32), r4.s32);
  }
}

// 0x82631E78: after the form's own handler (vtable[32]) answered (r3).
void mcla_tsp_form_key_res(PPCRegister& r3) {
  if (Window()) {
    REXLOG_INFO("TS form handler answered {}", r3.u32 & 0xFF);
  }
}

// 0x8268EE10: an event is posted (r3 = from, r4 = the code, r5 = 1 pressed / 3, r6 = -1).
void mcla_tsp_event(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  if (!Probe()) {
    return;
  }
  g_events.fetch_add(1, std::memory_order_relaxed);
  if (Window()) {
    REXLOG_INFO("TS event posted: code={} arg={} from={:08X} (update {}) t={}", r4.s32, r5.s32,
                r3.u32, g_update.load(std::memory_order_relaxed), NowUs());
  }
}

// 0x821FAE10 in sub_821FAD78 (the UI manager's event handler): EVENT_START_SCREEN_EXIT arrived.
void mcla_tsp_exit_event(PPCRegister& r4) {
  if (Probe()) {
    REXLOG_INFO("TS EVENT_START_SCREEN_EXIT handled (update {}) t={} event={:08X}",
                g_update.load(std::memory_order_relaxed), NowUs(), r4.u32);
  }
}

// 0x822CC808: the script native MoviePlayer_StopMovie; 0x822CBEF8: the start of a movie.
void mcla_tsp_movie_stop() {
  if (Probe()) {
    REXLOG_INFO("TS movie player: StopMovie (update {}) t={}",
                g_update.load(std::memory_order_relaxed), NowUs());
  }
}

void mcla_tsp_movie_start() {
  if (Probe()) {
    REXLOG_INFO("TS movie player: StartMovie (update {}) t={}",
                g_update.load(std::memory_order_relaxed), NowUs());
  }
}

// Counters.
void mcla_tsp_ui_update() {
  if (Probe()) {
    g_ui_updates.fetch_add(1, std::memory_order_relaxed);
  }
}

// 0x8260B990: the input mapper update (r3 = the mapper object).
void mcla_tsp_mapper(PPCRegister& r3) {
  if (!Probe()) {
    return;
  }
  g_mapper_calls.fetch_add(1, std::memory_order_relaxed);
  if (Window()) {
    REXLOG_INFO("TS mapper update {:08X} (update {})", r3.u32,
                g_update.load(std::memory_order_relaxed));
  }
}

// 0x825EDC08: a movie is stepped.
void mcla_tsp_movie_adv() {
  if (Probe()) {
    g_movie_adv.fetch_add(1, std::memory_order_relaxed);
  }
}

// 0x825EDCB0: one movie frame is run (r31 = the movie).
void mcla_tsp_movie_tick(PPCRegister& r31) {
  if (!Probe()) {
    return;
  }
  g_movie_ticks.fetch_add(1, std::memory_order_relaxed);
  if (Window()) {
    REXLOG_INFO("TS movie tick {:08X} (update {})", r31.u32,
                g_update.load(std::memory_order_relaxed));
  }
}

// 0x822C7FC8: the script native "button just pressed" is called (r4 = the button; 11 = Start).
void mcla_tsp_native_entry(PPCRegister& r4) {
  if (!Probe()) {
    return;
  }
  g_native_entry.fetch_add(1, std::memory_order_relaxed);
  if (r4.u32 == 11) {
    g_native_start.fetch_add(1, std::memory_order_relaxed);
  }
}

// 0x822C7FFC: the native got past the "a modal scene blocks input" check.
void mcla_tsp_native_pass() {
  if (Probe()) {
    g_native_pass.fetch_add(1, std::memory_order_relaxed);
  }
}

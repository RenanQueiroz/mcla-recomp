// Title screen input: the fix for Start having to be pressed many times on the title screen and
// the attract video (fork issue; owner's report 2026-10-29), and a development trace.
//
// What the image does with a controller (read in the recompiled code, not changed by the fork):
// once per game update sub_82614D30 reads XInput into a 64-byte pad record (pad n at
// 0x82905388 + 64 n): the word at +12 is the previous update's button mask, the word at +8 the
// new one (LB 4, RB 8, Y 16, B 32, A 64, X 128, Back 256, left stick 512, right stick 1024,
// Start 2048, up 4096, right 8192, down 16384, left 32768; the script button number is the bit
// number, Start = 11). A press is "just pressed" for ONE update: (+8 xor +12) and +8.
//
// The title screen and the attract video are decided by a game SCRIPT thread, not by the menus'
// key path (the UI input object, sub_82223CD8, turns the press into a key event that the scene
// stack takes but the title does not use). The thread asks the script native IS_PAD_BUTTON_DOWN
// (sub_822C8AF8: the "just pressed" test above, 8 buttons a loop, Start among them) and runs its
// loop only about every 25 to 33 ms: every 3rd game update at 120 FPS, every 2nd at 60, every update
// at 30. A press that comes in an update in which the thread does not run is gone by the next run:
// the chance a press counts is about 1/3 at 120 FPS (measured: 25 of 25 presses with a thread run
// in the press's update were accepted, 0 of 27 without), which is "press Start several times".
//
// mcla_script_pad_latch_ms: a thread that polls a button and misses its press in the update it
// happened in is told about the press once, at its next poll, if that is within this many
// milliseconds (see mcla_script_pad_read).
//
// mcla_pad_probe (development): logs, with the time and the update number of the pad, every
// button edge of the pad records, and for 1.5 s after each Start press the decisions of that key
// path (which of the three ways took the key, the main form's child, events posted, the movie
// player's start and stop, how many times the movies ticked and the input mapper ran in each
// update), and the script side: every read of Start by a script thread (which thread, whether it
// got the real edge or the latched one), the runs and updates of the threads that poll the pad.
// Once a second it prints the counts. Every line starts with "TS ". The vtable addresses are the
// class (the report script reads them from a table made from the image's RTTI).
//
// mcla_title_fps: while the title screen, the start-up and the attract video run (the lighting
// set-up that comes with the city has not run yet: the guard bit at 0x82836A28 is 0), the
// frame-rate cap is this many frames a second, except for a short time after a Start press (so
// that the load after it runs at the player's cap). A coarse workaround for the same problem: at
// 30 updates a second the script thread runs in every update.

#include "generated/default/mcla_init.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include <fmt/format.h>

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
REXCVAR_DEFINE_INT32(mcla_script_pad_latch_ms, 0, "MCLA",
                     "On the title screen and the attract video a script that polls a button "
                     "and misses its press (it runs only every 2nd to 4th update above 30 FPS) "
                     "is told about the press once, at its next poll, up to this many "
                     "milliseconds later (0 = as the game does: the press is seen only by a "
                     "script run in the same update)")
    .range(0, 500);

namespace {
constexpr uint32_t kPadBase = 0x82905388;
constexpr uint32_t kPadStride = 64;
constexpr uint32_t kPadCount = 4;
constexpr uint32_t kStartBit = 2048;
// The script button number of Start (the bit number in the pad record's masks).
constexpr uint32_t kStartButton = 11;
// scrThread::Run (sub_82554E20) stores the thread it runs here (and the previous one back after).
// In the thread: +8 the program, +16 the program counter of the opcode being run.
constexpr uint32_t kCurrentThread = 0x828DA3BC;
constexpr uint32_t kThreadProgram = 8;
constexpr uint32_t kThreadPc = 16;
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

// The number of modal scenes the UI manager has (the ring of three at +0x40724).
uint32_t ModalCount() {
  const uint32_t g1 = ReadGuestU32(kUiManagerPtr);
  const uint32_t impl = g1 ? ReadGuestU32(g1 + 52) : 0;
  return impl ? ReadGuestU32(impl + 0x40738) : 0;
}

// The vtable of the main form's current child form ([[main form + 68] + 436]).
uint32_t ChildFormVtable() {
  const uint32_t g2 = ReadGuestU32(kUiLogicPtr);
  const uint32_t array = g2 ? ReadGuestU32(g2 + 0x13F80 + 68) : 0;
  return Vtable(array ? ReadGuestU32(array + 436) : 0);
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
    g_native_entry{0}, g_native_start{0}, g_native_pass{0}, g_events{0}, g_dispatch{0},
    g_action_queries{0};

// Only the game's thread (the one that runs the updates) touches these.
std::array<uint64_t, kPadCount> g_updates{};

// ---- The latch for script threads (mcla_script_pad_latch_ms) ----
// Per pad and button: how many presses the pad record has shown, and when the last one came.
// Written by the pad update hook, read by the native's hook (both run on the game's thread; the
// mutex is for any other reader).
constexpr uint32_t kButtons = 16;
// A script thread polls a button if it read it within this long before the press.
constexpr uint64_t kPollerUs = 200'000;
std::mutex g_latch_mutex;
std::array<std::array<uint32_t, kButtons>, kPadCount> g_edge_seq{};
std::array<std::array<uint64_t, kButtons>, kPadCount> g_edge_us{};

// What one script call site has done with one (pad, button). A call site is a place in a script's
// code (the thread's program and program counter), not a thread: it is the same whichever thread
// runs it, and still the same if the script starts a new thread for each pass of its loop.
struct PollState {
  uint64_t last_poll_us = 0;  // its last read of this button
  uint32_t seen_seq = 0;      // the press it has been told about (the real edge or the latched one)
  uint64_t seen_update = 0;   // the pad update it was told in
  bool seen_latched = false;  // ... by the latch (later reads in that update answer the same)
};
struct SitePolls {
  uint64_t site = 0;  // (program << 32 | pc) with the top bit set; 0 = the slot is free
  uint64_t last_any_poll_us = 0;
  std::array<std::array<PollState, kButtons>, kPadCount> state{};
};
constexpr size_t kSiteSlots = 128;
std::array<SitePolls, kSiteSlots> g_sites{};

// The slot of a call site (the one that polled longest ago is replaced when all are used).
// Called with g_latch_mutex held.
SitePolls& SiteSlot(uint64_t site) {
  size_t oldest = 0;
  for (size_t i = 0; i < kSiteSlots; i++) {
    if (g_sites[i].site == site) {
      return g_sites[i];
    }
    if (g_sites[i].last_any_poll_us < g_sites[oldest].last_any_poll_us) {
      oldest = i;
    }
  }
  g_sites[oldest] = SitePolls{};
  g_sites[oldest].site = site;
  return g_sites[oldest];
}

// The script threads that read the pad in the last second (for the trace of their runs).
constexpr size_t kPollThreads = 32;
std::array<std::pair<uint32_t, uint64_t>, kPollThreads> g_poll_threads{};

// Called with g_latch_mutex held.
void NotePollThread(uint32_t thread, uint64_t now) {
  size_t oldest = 0;
  for (size_t i = 0; i < kPollThreads; i++) {
    if (g_poll_threads[i].first == thread) {
      g_poll_threads[i].second = now;
      return;
    }
    if (g_poll_threads[i].second < g_poll_threads[oldest].second) {
      oldest = i;
    }
  }
  g_poll_threads[oldest] = {thread, now};
}

bool IsPadPoller(uint32_t thread, uint64_t now) {
  std::lock_guard<std::mutex> lock(g_latch_mutex);
  for (const auto& t : g_poll_threads) {
    if (t.first == thread) {
      return t.second && now - t.second < 1'000'000;
    }
  }
  return false;
}

std::atomic<uint64_t> g_start_reads{0}, g_start_real{0}, g_start_latched{0}, g_pad_reads{0},
    g_thread_runs{0}, g_thread_updates{0};
// Reads of buttons other than Start logged in this window (the first few after each Start press:
// they show which buttons the script asks about and so that Start is button 11).
std::atomic<uint32_t> g_other_reads_logged{0};

// The script interpreter's native calls (sub_82554E20, the call at 0x82555CC8): how often each
// native function was called in the last second (a small table, probed linearly), and the first
// ones after each Start press.
constexpr size_t kNativeSlots = 512;
std::atomic<uint32_t> g_native_fn[kNativeSlots];
std::atomic<uint32_t> g_native_count[kNativeSlots];
std::atomic<uint32_t> g_native_window_logged{0};

void CountNative(uint32_t fn) {
  for (size_t i = 0, slot = (fn >> 2) % kNativeSlots; i < kNativeSlots; i++, slot = (slot + 1) % kNativeSlots) {
    uint32_t seen = g_native_fn[slot].load(std::memory_order_relaxed);
    if (seen == fn) {
      g_native_count[slot].fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (seen == 0) {
      uint32_t expected = 0;
      if (g_native_fn[slot].compare_exchange_strong(expected, fn, std::memory_order_relaxed) || expected == fn) {
        g_native_count[slot].fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
}

// The five most called natives of the last second as "XXXXXXXX x N", and the table cleared.
std::string TopNatives() {
  std::array<std::pair<uint32_t, uint32_t>, kNativeSlots> all{};
  size_t n = 0;
  for (size_t i = 0; i < kNativeSlots; i++) {
    const uint32_t fn = g_native_fn[i].load(std::memory_order_relaxed);
    if (!fn) {
      continue;
    }
    all[n++] = {g_native_count[i].exchange(0, std::memory_order_relaxed), fn};
  }
  std::sort(all.begin(), all.begin() + n, [](auto& a, auto& b) { return a.first > b.first; });
  std::string out;
  for (size_t i = 0; i < n && i < 8 && all[i].first; i++) {
    out += fmt::format("{:08X} x{} ", all[i].second, all[i].first);
  }
  return out.empty() ? std::string("none") : out;
}

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
      "state {}, city pending {}) modal scenes {} child form vtable {:08X} action_queries {} script natives: {}",
      now, g_updates[0] - g_report_updates, g_updates[0], g_ui_updates.exchange(0),
      g_mapper_calls.exchange(0), g_movie_adv.exchange(0), g_movie_ticks.exchange(0),
      g_native_entry.exchange(0), g_native_start.exchange(0), g_native_pass.exchange(0),
      g_events.exchange(0), g_dispatch.exchange(0), InTitlePhase(), ReadGuestU32(kGameState),
      ReadGuestU32(kCityPending), ModalCount(), ChildFormVtable(), g_action_queries.exchange(0),
      TopNatives());
  g_report_updates = g_updates[0];
  REXLOG_INFO(
      "TS 1s-script t={} start_reads={} start_real={} start_latched={} pad_reads={} thread_runs={} "
      "thread_updates={} latch_ms={}",
      now, g_start_reads.exchange(0), g_start_real.exchange(0), g_start_latched.exchange(0),
      g_pad_reads.exchange(0), g_thread_runs.exchange(0), g_thread_updates.exchange(0),
      REXCVAR_GET(mcla_script_pad_latch_ms));
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
  const int32_t latch_ms = REXCVAR_GET(mcla_script_pad_latch_ms);
  if (!probe && latch_ms <= 0 && REXCVAR_GET(mcla_title_fps) <= 0) {
    return;
  }
  const uint64_t now = NowUs();
  const uint64_t update = ++g_updates[pad];
  const uint32_t cur = ReadGuestU32(r31.u32 + 8);
  const uint32_t last = ReadGuestU32(r31.u32 + 12);
  const uint32_t pressed = cur & ~last;
  const uint32_t released = last & ~cur;
  if (pad == 0) {
    g_update.store(update, std::memory_order_relaxed);
  }
  if (pressed) {
    // Count the presses for the script latch: the native sees each one for this update only.
    std::lock_guard<std::mutex> lock(g_latch_mutex);
    for (uint32_t b = 0; b < kButtons; b++) {
      if (pressed & (1u << b)) {
        g_edge_seq[pad][b]++;
        g_edge_us[pad][b] = now;
      }
    }
  }
  if (pressed & kStartBit) {
    g_last_start_us.store(now, std::memory_order_relaxed);
    if (probe) {
      g_window_until_us.store(now + kWindowUs, std::memory_order_relaxed);
      g_native_window_logged.store(0, std::memory_order_relaxed);
      g_other_reads_logged.store(0, std::memory_order_relaxed);
    }
  }
  if (probe && (pressed || released)) {
    REXLOG_INFO("TS pad{} edge t={} update={} down={:#06x} up={:#06x} cur={:#06x}", pad, now,
                update, pressed, released, cur);
    if (pressed & kStartBit) {
      LogState(pad, update);
    }
  }
  if (probe && pad == 0) {
    MaybeReport(now);
  }
}

// 0x822C8B34 in sub_822C8AF8, the script native IS_PAD_BUTTON_DOWN(pad, button), which is really
// "this button was pressed in this update": the instruction `cntlzw r6,r8` that turns r8 (the press:
// ((+8 xor +12) and the button's bit) and +8, nonzero = pressed) into the answer. r6 = the pad,
// r5 = the button (its bit number).
//
// A script thread that runs only every 2nd to 4th update (the title's, above 30 FPS) asks in about
// one update in 3 at 120 FPS and so misses most presses. With mcla_script_pad_latch_ms > 0 and the
// title screen up, a call site in a script (program and program counter of the thread) that is not
// given the real press is told about it once, at its next poll, when
//  - it polled this button within 200 ms before the press (it is a poller: a call site that starts
//    asking after the press is not told, so one press does not also answer the next screen),
//  - it has not been told about this press (by the real edge, or earlier),
//  - the press is at most mcla_script_pad_latch_ms old.
// Later polls in the update it was told in answer the same, as they would for the real edge.
void mcla_script_pad_read(PPCRegister& r8, PPCRegister& r6, PPCRegister& r5) {
  const int32_t latch_ms = REXCVAR_GET(mcla_script_pad_latch_ms);
  const bool probe = Probe();
  if (latch_ms <= 0 && !probe) {
    return;
  }
  const uint32_t pad = r6.u32;
  const uint32_t button = r5.u32;
  if (pad >= kPadCount || button >= kButtons) {
    return;
  }
  const uint32_t thread = ReadGuestU32(kCurrentThread);
  const uint32_t program = thread ? ReadGuestU32(thread + kThreadProgram) : 0;
  const uint32_t pc = thread ? ReadGuestU32(thread + kThreadPc) : 0;
  const uint64_t site = (uint64_t(program) << 32) | pc | (uint64_t(1) << 63);
  const uint64_t now = NowUs();
  const bool real = (r8.u32 != 0);
  bool latched = false;
  uint64_t age_us = 0;
  uint64_t before_us = 0;
  {
    std::lock_guard<std::mutex> lock(g_latch_mutex);
    SitePolls& t = SiteSlot(site);
    PollState& st = t.state[pad][button];
    const uint32_t seq = g_edge_seq[pad][button];
    const uint64_t edge_us = g_edge_us[pad][button];
    const uint64_t update = g_updates[pad];
    if (real) {
      st.seen_seq = seq;
      st.seen_update = update;
      st.seen_latched = false;
    } else if (latch_ms > 0 && seq != 0) {
      if (st.seen_seq == seq) {
        latched = st.seen_latched && st.seen_update == update;
      } else if (InTitlePhase() && st.last_poll_us != 0 && st.last_poll_us < edge_us &&
                 edge_us - st.last_poll_us <= kPollerUs &&
                 now - edge_us <= uint64_t(latch_ms) * 1000) {
        latched = true;
        st.seen_seq = seq;
        st.seen_update = update;
        st.seen_latched = true;
      }
    }
    age_us = edge_us ? now - edge_us : 0;
    before_us = st.last_poll_us ? now - st.last_poll_us : 0;
    st.last_poll_us = now;
    t.last_any_poll_us = now;
    NotePollThread(thread, now);
  }
  if (latched) {
    r8.u64 = 1;
  }
  if (!probe) {
    return;
  }
  g_pad_reads.fetch_add(1, std::memory_order_relaxed);
  if (button == kStartButton) {
    g_start_reads.fetch_add(1, std::memory_order_relaxed);
    if (real) {
      g_start_real.fetch_add(1, std::memory_order_relaxed);
    }
    if (latched) {
      g_start_latched.fetch_add(1, std::memory_order_relaxed);
    }
    if (Window()) {
      REXLOG_INFO(
          "TS padread thread={:08X} program={:08X} pc={} pad={} button={} real={} latched={} update={} "
          "t={} press_age_us={} previous_poll_us_ago={}",
          thread, program, pc, pad, button, real ? 1 : 0, latched ? 1 : 0,
          g_update.load(std::memory_order_relaxed), now, age_us, before_us);
    }
  } else if (Window() && g_other_reads_logged.fetch_add(1, std::memory_order_relaxed) < 48) {
    REXLOG_INFO(
        "TS padread thread={:08X} program={:08X} pc={} pad={} button={} real={} latched={} update={} "
        "t={} press_age_us={} previous_poll_us_ago={}",
        thread, program, pc, pad, button, real ? 1 : 0, latched ? 1 : 0,
        g_update.load(std::memory_order_relaxed), now, age_us, before_us);
  }
}

// 0x82554E20: scrThread::Run (sub_82554E20; r3 = the thread). 0x82553C10: scrThread::Update
// (vtable[3]: adds 16 ms to the thread's clocks and calls Run). Logged in the window after a Start
// press for the threads that poll the pad, to see how often they run against the game's updates.
void mcla_tsp_thread_run(PPCRegister& r3) {
  if (!Probe()) {
    return;
  }
  g_thread_runs.fetch_add(1, std::memory_order_relaxed);
  if (Window()) {
    const uint64_t now = NowUs();
    if (IsPadPoller(r3.u32, now)) {
      REXLOG_INFO("TS thread run {:08X} update={} t={}", r3.u32,
                  g_update.load(std::memory_order_relaxed), now);
    }
  }
}

void mcla_tsp_thread_update(PPCRegister& r3) {
  if (!Probe()) {
    return;
  }
  g_thread_updates.fetch_add(1, std::memory_order_relaxed);
  if (Window()) {
    const uint64_t now = NowUs();
    if (IsPadPoller(r3.u32, now)) {
      REXLOG_INFO("TS thread update {:08X} update={} t={}", r3.u32,
                  g_update.load(std::memory_order_relaxed), now);
    }
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
    REXLOG_INFO("TS way A: modal scene {:08X} vtable {:08X} handler {:08X} (vtable[32] {:08X}) key={}",
                r3.u32, r8.u32, ReadGuestU32(r8.u32 + 276), ReadGuestU32(r8.u32 + 32), r4.s32);
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
    REXLOG_INFO("TS way B2: user interface {:08X} vtable {:08X} handler {:08X} (vtable[32] {:08X}) key={}",
                r3.u32, r11.u32, ReadGuestU32(r11.u32 + 276), ReadGuestU32(r11.u32 + 32), r4.s32);
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
    REXLOG_INFO("TS main form -> child {:08X} vtable {:08X} handler {:08X} (vtable[32] {:08X}) key={}",
                r3.u32, r11.u32, ReadGuestU32(r11.u32 + 276), ReadGuestU32(r11.u32 + 32), r4.s32);
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

// 0x8263B010 in sub_8263AFA8 (the key handler UI objects share: it offers the key to each active
// child in turn, depth first, until one takes it): r31 = the child, r4 = the key, r11 = its
// vtable; the call is vtable[32]. These lines are the focus chain down to the object that takes
// the key.
void mcla_tsp_focus_call(PPCRegister& r31, PPCRegister& r4, PPCRegister& r11) {
  if (Window()) {
    REXLOG_INFO("TS focus: child {:08X} vtable {:08X} handler {:08X} key={}", r31.u32, r11.u32,
                ReadGuestU32(r11.u32 + 32), r4.s32);
  }
}

// 0x8263B01C: after the child's call: r3 = its answer, r31 = the child.
void mcla_tsp_focus_res(PPCRegister& r3, PPCRegister& r31) {
  if (Window()) {
    REXLOG_INFO("TS focus: child {:08X} answered {}", r31.u32, r3.u32 & 0xFF);
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

// 0x82555CC4 in sub_82554E20 (the script interpreter): `mtctr r11` before the call of a native
// function (r11 = it).
void mcla_tsp_script_native(PPCRegister& r11) {
  if (!Probe()) {
    return;
  }
  CountNative(r11.u32);
  if (Window() && g_native_window_logged.fetch_add(1, std::memory_order_relaxed) < 200) {
    REXLOG_INFO("TS native {:08X} (update {})", r11.u32, g_update.load(std::memory_order_relaxed));
  }
}

// 0x8221D660: "is UI action n down" (sub_8221D660, r3 = n; the UI input object's records, Start =
// 7). Called by the spectator's event listener at least; the trace shows whether anything asks
// during the title.
void mcla_tsp_action_down(PPCRegister& r3) {
  if (!Probe()) {
    return;
  }
  g_action_queries.fetch_add(1, std::memory_order_relaxed);
  if (r3.u32 == 7 && Window()) {
    REXLOG_INFO("TS action 7 (Start) queried (update {}) t={}",
                g_update.load(std::memory_order_relaxed), NowUs());
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

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
// milliseconds (see ScriptPoll). It covers every script native that asks "pressed in this update":
// IS_PAD_BUTTON_DOWN and IS_BUTTON_DOWN (the pad record), UILogic_IsPressed and
// Player_FlashingHighbeams (an action record of the player's controls, sub_822A4CE0);
// mcla_script_pad_latch_scope says where (every script, the default, or only the title screen
// and the attract video).
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
#include <unordered_map>
#include <utility>
#include <vector>

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
REXCVAR_DEFINE_INT32(mcla_script_pad_latch_ms, 100, "MCLA",
                     "A script that polls a button and misses its press (it runs only every 2nd "
                     "to 4th update above 30 FPS) is told about the press once, at its next poll, "
                     "up to this many milliseconds later (0 = off, as the game does: the press is "
                     "seen only by a script run in the same update)")
    .range(0, 500);
REXCVAR_DEFINE_STRING(mcla_script_pad_latch_scope, "all", "MCLA",
                      "Where mcla_script_pad_latch_ms applies: all (every script) or title (only "
                      "the title screen and the attract video)");

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
// A script native that asks "was this button pressed in this update" is true for ONE update, and a script
// thread that runs only every 2nd to 4th update (above 30 FPS) asks in one update in 3 at 120 FPS and
// misses most presses. An EDGE SOURCE is the thing such a native asks about:
//  - a pad's button (IS_PAD_BUTTON_DOWN, IS_BUTTON_DOWN: the pad record's masks, sub_82614D30 updates
//    them once per game update; the press is seen in the pad update hook);
//  - an action of the player's controls (UILogic_IsPressed, Player_FlashingHighbeams: sub_822A4CE0 on the
//    12-byte record at [controls + 12] + 12 * action). Nothing updates a press counter for these, so the
//    pad update hook samples the records the scripts have asked about, once per game update, and a real
//    press that a script reads first counts at once.
// For each source the latch counts the presses (a sequence number and the time of the last one). For
// each script CALL SITE (the running thread's program and program counter) and source it keeps the
// press it has been told about, and when it last asked.
//
// Wall-clock windows: a call site is a poller when it asked within kPollerUs before the press, and a
// missed press is told within mcla_script_pad_latch_ms after it.
// The pad record's button bits that scripts ask about (the script button number is the bit number).
constexpr uint32_t kButtons = 16;
constexpr uint64_t kSrcPad = uint64_t(1) << 56;
constexpr uint64_t kSrcRecord = uint64_t(2) << 56;
inline uint64_t SrcPad(uint32_t pad, uint32_t button) {
  return kSrcPad | (uint64_t(pad) << 16) | button;
}
inline uint64_t SrcRecord(uint32_t controls, uint32_t action) {
  return kSrcRecord | (uint64_t(controls) << 16) | (action & 0xFFFF);
}
inline bool IsRecordSrc(uint64_t src) {
  return (src >> 56) == 2;
}
std::string SrcName(uint64_t src) {
  if (IsRecordSrc(src)) {
    return fmt::format("ctl{:08X}/a{}", uint32_t(src >> 16), uint32_t(src & 0xFFFF));
  }
  return fmt::format("pad{}/b{}", uint32_t((src >> 16) & 0xFF), uint32_t(src & 0xFFFF));
}

// A script thread polls a source if it asked within this long before the press.
constexpr uint64_t kPollerUs = 200'000;
// An audit (development) waits this long after a press for the call sites to be told.
constexpr uint64_t kAuditUs = 600'000;
constexpr size_t kMaxPolls = 6000;

std::mutex g_latch_mutex;

struct EdgeInfo {
  uint32_t seq = 0;       // presses seen
  uint64_t us = 0;        // when the last one was seen
  bool flag = false;      // record sources: the record said "pressed" at the last look
  bool scanned = false;   // record sources: in g_record_srcs
  uint32_t controls = 0;  // record sources
  uint32_t action = 0;
  uint64_t last_use_us = 0;
};
std::unordered_map<uint64_t, EdgeInfo> g_edges;
std::vector<uint64_t> g_record_srcs;  // the record sources to look at in each update

struct PollKey {
  uint64_t site = 0;  // (program << 32 | pc) with the top bit set
  uint64_t src = 0;
  bool operator==(const PollKey& o) const { return site == o.site && src == o.src; }
};
struct PollKeyHash {
  size_t operator()(const PollKey& k) const {
    return size_t((k.site * 0x9E3779B97F4A7C15ull) ^ (k.src + 0x7F4A7C15ull + (k.site >> 7)));
  }
};

// What one script call site has done with one source.
struct PollState {
  uint64_t last_poll_us = 0;  // its last ask
  uint32_t seen_seq = 0;      // the press it has been told about (the real one or the latched one)
  uint64_t seen_update = 0;   // the game update it was told in
  bool seen_latched = false;  // ... by the latch (later asks in that update answer the same)
  // Trace and audit (development).
  uint64_t total_reads = 0;
  uint32_t reads = 0, real = 0, latched = 0;  // since the last 1 s line
  uint32_t program = 0, pc = 0;
  const char* native = "";
  bool audit_pending = false;  // a press came while this call site was polling
  uint32_t audit_seq = 0;
  uint64_t audit_press_us = 0;
  uint32_t told = 0;  // the game updates it was told about that press in
  uint64_t told_update = 0;
  uint64_t told_first_us = 0;
  bool told_first_latched = false;
};
std::unordered_map<PollKey, PollState, PollKeyHash> g_polls;

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

// The audit (needs mcla_pad_probe): for each press, the call sites that were polling the source
// when it came, whether each was told about it (the real press or the latched one), and how many
// times. A call site told twice would act twice.
std::atomic<uint64_t> g_audit_eligible{0}, g_audit_saw{0}, g_audit_missed{0}, g_audit_doubled{0},
    g_audit_gone{0};

// Called with g_latch_mutex held.
void FinalizeAuditLocked(const PollKey& key, PollState& st) {
  if (!st.audit_pending) {
    return;
  }
  st.audit_pending = false;
  const bool polled_after = st.last_poll_us > st.audit_press_us;
  const char* verdict = st.told >= 2 ? "DOUBLED"
                        : st.told == 1 ? "saw"
                        : polled_after ? "MISSED"
                                       : "gone";
  g_audit_eligible.fetch_add(1, std::memory_order_relaxed);
  if (st.told >= 2) {
    g_audit_doubled.fetch_add(1, std::memory_order_relaxed);
  }
  if (st.told >= 1) {
    g_audit_saw.fetch_add(1, std::memory_order_relaxed);
  } else if (polled_after) {
    g_audit_missed.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_audit_gone.fetch_add(1, std::memory_order_relaxed);
  }
  REXLOG_INFO(
      "TS audit native={} program={:08X} pc={} src={} press_seq={} verdict={} told_updates={} "
      "first={} delay_us={} polled_after={}",
      st.native, st.program, st.pc, SrcName(key.src), st.audit_seq, verdict, st.told,
      st.told ? (st.told_first_latched ? "latched" : "real") : "-",
      st.told ? st.told_first_us - st.audit_press_us : 0, polled_after ? 1 : 0);
}

// A new press of a source (called with g_latch_mutex held).
void RegisterEdgeLocked(uint64_t src, EdgeInfo& e, uint64_t now) {
  e.seq++;
  e.us = now;
  if (!Probe()) {
    return;
  }
  for (auto& kv : g_polls) {
    if (kv.first.src != src) {
      continue;
    }
    PollState& st = kv.second;
    FinalizeAuditLocked(kv.first, st);
    if (st.last_poll_us != 0 && now - st.last_poll_us <= kPollerUs) {
      st.audit_pending = true;
      st.audit_seq = e.seq;
      st.audit_press_us = now;
      st.told = 0;
      st.told_update = 0;
      st.told_first_us = 0;
    }
  }
}

// The audits whose time is up.
void SweepAudits(uint64_t now) {
  std::lock_guard<std::mutex> lock(g_latch_mutex);
  for (auto& kv : g_polls) {
    if (kv.second.audit_pending && now - kv.second.audit_press_us > kAuditUs) {
      FinalizeAuditLocked(kv.first, kv.second);
    }
  }
}

void PruneLocked(uint64_t now) {
  for (auto it = g_polls.begin(); it != g_polls.end();) {
    if (!it->second.audit_pending && now - it->second.last_poll_us > 5'000'000) {
      it = g_polls.erase(it);
    } else {
      ++it;
    }
  }
}

// Whether the action record says "pressed in this update": sub_822A4CE0's test. A record is 12
// bytes at [controls + 12] + 12 * action: byte 0 and byte 2 differ in bit 7 and byte 3 agrees with
// byte 0 in bit 7.
bool RecordPressed(uint32_t controls, uint32_t action) {
  const uint32_t base = ReadGuestU32(controls + 12);
  if (!base) {
    return false;
  }
  const uint32_t rec = base + 12 * action;
  const uint32_t b0 = ReadGuestU8(rec), b2 = ReadGuestU8(rec + 2), b3 = ReadGuestU8(rec + 3);
  return ((b0 ^ b2) & 0x80) != 0 && ((b3 ^ b0) & 0x80) == 0;
}

// Once per game update (pad 0's update hook): look at the action records the scripts ask about.
void ScanRecordSources(uint64_t now) {
  std::lock_guard<std::mutex> lock(g_latch_mutex);
  for (size_t i = 0; i < g_record_srcs.size();) {
    const uint64_t src = g_record_srcs[i];
    EdgeInfo& e = g_edges[src];
    if (now - e.last_use_us > 10'000'000) {  // nobody asks about it any more
      e.scanned = false;
      g_record_srcs[i] = g_record_srcs.back();
      g_record_srcs.pop_back();
      continue;
    }
    const bool flag = RecordPressed(e.controls, e.action);
    if (flag && !e.flag) {
      RegisterEdgeLocked(src, e, now);
    }
    e.flag = flag;
    i++;
  }
}

// Once a second (mcla_pad_probe): the call sites that asked in the last second.
void ReportSites(uint64_t now) {
  struct Row {
    uint32_t reads, real, latched, program, pc;
    const char* native;
    uint64_t src;
  };
  std::vector<Row> rows;
  {
    std::lock_guard<std::mutex> lock(g_latch_mutex);
    for (auto& kv : g_polls) {
      PollState& st = kv.second;
      if (st.reads) {
        rows.push_back({st.reads, st.real, st.latched, st.program, st.pc, st.native, kv.first.src});
      }
      st.reads = st.real = st.latched = 0;
    }
  }
  if (rows.empty()) {
    return;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.reads > b.reads; });
  std::string line;
  for (size_t i = 0; i < rows.size() && i < 40; i++) {
    const Row& r = rows[i];
    line += fmt::format(" | {} {:08X}:{} {} r{} e{} l{}", r.native, r.program, r.pc, SrcName(r.src),
                        r.reads, r.real, r.latched);
  }
  REXLOG_INFO("TS sites t={} n={} audits: eligible={} saw={} missed={} doubled={} gone={}{}", now,
              rows.size(), g_audit_eligible.load(), g_audit_saw.load(), g_audit_missed.load(),
              g_audit_doubled.load(), g_audit_gone.load(), line);
}

bool ScopeAll() {
  const std::string scope = REXCVAR_GET(mcla_script_pad_latch_scope);
  return scope == "all";
}

// Whether the latch has anything to do now: it is on, and the scope is every script or the title
// phase is up. Outside that nothing is counted (the hooks cost one cvar read and a guard read).
bool LatchWanted() {
  return REXCVAR_GET(mcla_script_pad_latch_ms) > 0 && (ScopeAll() || InTitlePhase());
}

struct PollOut {
  bool latched = false;    // tell the script the button was pressed
  bool first = false;      // the first ask of this source by this call site
  uint64_t age_us = 0;     // since the source's last press
  uint64_t before_us = 0;  // since this call site's previous ask
};

// A script call site asks about a source; `real` is the native's own answer. Returns whether the
// answer is to be changed to "pressed": a call site that is not given the real press is told about
// it once, at its next poll, when
//  - it asked this source within kPollerUs before the press (it is a poller: a call site that starts
//    asking after the press is not told, so one press does not also answer the next screen),
//  - it has not been told about this press (by the real answer, or earlier),
//  - the press is at most latch_ms old,
//  - the scope allows it (the title screen, or every script).
// Later asks in the update it was told in answer the same, as they would for the real press.
PollOut ScriptPoll(uint64_t site, uint64_t src, const char* native, uint32_t program, uint32_t pc,
                   bool real, int32_t latch_ms, uint32_t controls, uint32_t action) {
  PollOut out;
  const uint64_t now = NowUs();
  const uint64_t update = g_update.load(std::memory_order_relaxed);
  const bool in_scope = latch_ms > 0 && (ScopeAll() || InTitlePhase());
  std::lock_guard<std::mutex> lock(g_latch_mutex);
  if (g_polls.size() > kMaxPolls) {
    PruneLocked(now);
  }
  EdgeInfo& e = g_edges[src];
  PollState& st = g_polls[PollKey{site, src}];
  out.first = st.total_reads == 0;
  if (IsRecordSrc(src)) {
    e.controls = controls;
    e.action = action;
    e.last_use_us = now;
    if (!e.scanned) {
      e.scanned = true;
      g_record_srcs.push_back(src);
    }
    if (real && !e.flag) {
      // A press that the look at the start of the update has not seen (the scripts ran after the
      // controls were updated): count it now.
      e.flag = true;
      RegisterEdgeLocked(src, e, now);
    }
  }
  const uint32_t seq = e.seq;
  if (real) {
    st.seen_seq = seq;
    st.seen_update = update;
    st.seen_latched = false;
  } else if (latch_ms > 0 && seq != 0) {
    if (st.seen_seq == seq) {
      out.latched = st.seen_latched && st.seen_update == update;
    } else if (in_scope && st.last_poll_us != 0 && st.last_poll_us < e.us &&
               e.us - st.last_poll_us <= kPollerUs && now - e.us <= uint64_t(latch_ms) * 1000) {
      out.latched = true;
      st.seen_seq = seq;
      st.seen_update = update;
      st.seen_latched = true;
    }
  }
  if ((real || out.latched) && st.audit_pending && st.audit_seq == seq &&
      st.told_update != update) {
    st.told_update = update;
    if (++st.told == 1) {
      st.told_first_us = now;
      st.told_first_latched = out.latched;
    }
  }
  out.age_us = e.us ? now - e.us : 0;
  out.before_us = st.last_poll_us ? now - st.last_poll_us : 0;
  st.last_poll_us = now;
  st.total_reads++;
  st.reads++;
  if (real) {
    st.real++;
  }
  if (out.latched) {
    st.latched++;
  }
  st.program = program;
  st.pc = pc;
  st.native = native;
  return out;
}

// A script asked in a way that was refused (a modal scene blocks input, the player is not in a car):
// the call site is not polling for now, so a press made meanwhile is not told to it later.
void ScriptBlocked(uint64_t site) {
  std::lock_guard<std::mutex> lock(g_latch_mutex);
  for (auto& kv : g_polls) {
    if (kv.first.site == site) {
      kv.second.last_poll_us = 0;
    }
  }
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
  SweepAudits(now);
  ReportSites(now);
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
  const bool latch = LatchWanted();
  if (!probe && !latch && REXCVAR_GET(mcla_title_fps) <= 0) {
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
        const uint64_t src = SrcPad(pad, b);
        RegisterEdgeLocked(src, g_edges[src], now);
      }
    }
  }
  if (pad == 0 && (latch || probe)) {
    ScanRecordSources(now);  // the action records the scripts ask about
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

namespace {
// The call site of the script native being run: the running thread's program (+8) and the program
// counter of the opcode (+16; scrThread::Run stores it before each opcode).
struct ScriptSite {
  uint32_t thread = 0, program = 0, pc = 0;
  uint64_t key = 0;
};

ScriptSite CurrentScriptSite() {
  ScriptSite s;
  s.thread = ReadGuestU32(kCurrentThread);
  s.program = s.thread ? ReadGuestU32(s.thread + kThreadProgram) : 0;
  s.pc = s.thread ? ReadGuestU32(s.thread + kThreadPc) : 0;
  s.key = (uint64_t(s.program) << 32) | s.pc | (uint64_t(1) << 63);
  return s;
}

// The trace of a script ask (mcla_pad_probe): the first ask of each call site and source.
void TraceSiteNew(const char* native, const ScriptSite& s, uint64_t src) {
  REXLOG_INFO("TS site-new native={} program={:08X} (name hash {:08X}) pc={} src={} thread={:08X} t={} update={}",
              native, s.program, s.program ? ReadGuestU32(s.program + 4) : 0, s.pc, SrcName(src),
              s.thread, NowUs(), g_update.load(std::memory_order_relaxed));
}

// Common part of the pad-record natives (IS_PAD_BUTTON_DOWN, IS_BUTTON_DOWN): `real` is the native's
// answer; returns whether it is to be changed to "pressed".
bool PadNativePoll(const char* native, uint32_t pad, uint32_t button, bool real, bool log_start) {
  const int32_t latch_ms = REXCVAR_GET(mcla_script_pad_latch_ms);
  const bool probe = Probe();
  if (!probe && !LatchWanted()) {
    return false;
  }
  if (pad >= kPadCount || button >= kButtons) {
    return false;
  }
  const ScriptSite s = CurrentScriptSite();
  const uint64_t src = SrcPad(pad, button);
  const PollOut out = ScriptPoll(s.key, src, native, s.program, s.pc, real, latch_ms, 0, 0);
  {
    std::lock_guard<std::mutex> lock(g_latch_mutex);
    NotePollThread(s.thread, NowUs());
  }
  if (!probe) {
    return out.latched;
  }
  if (out.first) {
    TraceSiteNew(native, s, src);
  }
  if (!log_start) {
    return out.latched;
  }
  g_pad_reads.fetch_add(1, std::memory_order_relaxed);
  const uint64_t now = NowUs();
  if (button == kStartButton) {
    g_start_reads.fetch_add(1, std::memory_order_relaxed);
    if (real) {
      g_start_real.fetch_add(1, std::memory_order_relaxed);
    }
    if (out.latched) {
      g_start_latched.fetch_add(1, std::memory_order_relaxed);
    }
    if (Window()) {
      REXLOG_INFO(
          "TS padread thread={:08X} program={:08X} pc={} pad={} button={} real={} latched={} update={} "
          "t={} press_age_us={} previous_poll_us_ago={}",
          s.thread, s.program, s.pc, pad, button, real ? 1 : 0, out.latched ? 1 : 0,
          g_update.load(std::memory_order_relaxed), now, out.age_us, out.before_us);
    }
  } else if (Window() && g_other_reads_logged.fetch_add(1, std::memory_order_relaxed) < 48) {
    REXLOG_INFO(
        "TS padread thread={:08X} program={:08X} pc={} pad={} button={} real={} latched={} update={} "
        "t={} press_age_us={} previous_poll_us_ago={}",
        s.thread, s.program, s.pc, pad, button, real ? 1 : 0, out.latched ? 1 : 0,
        g_update.load(std::memory_order_relaxed), now, out.age_us, out.before_us);
  }
  return out.latched;
}
}  // namespace

// 0x822C8B34 in sub_822C8AF8, the script native IS_PAD_BUTTON_DOWN(pad, button), which is really
// "this button was pressed in this update": the instruction `cntlzw r6,r8` that turns r8 (the press:
// ((+8 xor +12) and the button's bit) and +8, nonzero = pressed) into the answer. r6 = the pad,
// r5 = the button (its bit number). See ScriptPoll for what the latch does.
void mcla_script_pad_read(PPCRegister& r8, PPCRegister& r6, PPCRegister& r5) {
  if (PadNativePoll("IS_PAD_BUTTON_DOWN", r6.u32, r5.u32, r8.u32 != 0, true)) {
    r8.u64 = 1;
  }
}

// 0x822C8030 in sub_822C7FC8 (IS_BUTTON_DOWN(player, button) = sub_822C8AB0 -> this), the same test
// after the "a modal scene blocks input" check: the instruction `cntlzw r11,r3`. r3 = the press, r30
// = the button, r11 = the pad record (the player's pad).
void mcla_script_button_read(PPCRegister& r3, PPCRegister& r30, PPCRegister& r11) {
  if (r11.u32 < kPadBase) {
    return;
  }
  if (PadNativePoll("IS_BUTTON_DOWN", (r11.u32 - kPadBase) / kPadStride, r30.u32, r3.u32 != 0,
                    false)) {
    r3.u64 = 1;
  }
}

// 0x822C7FF4 in sub_822C7FC8 (`li r3,0`, the modal scene blocks input) and 0x822A6DD4 in sub_822A6D48
// (Player_FlashingHighbeams answers no without asking the record): the call site is not polling now.
void mcla_script_blocked_button() {
  if (!Probe() && !LatchWanted()) {
    return;
  }
  ScriptBlocked(CurrentScriptSite().key);
}

void mcla_script_blocked_highbeams() {
  if (!Probe() && !LatchWanted()) {
    return;
  }
  ScriptBlocked(CurrentScriptSite().key);
}

namespace {
// The controls object and the action of the record test sub_822A4CE0 is about to make: the natives
// below call it with r3 = [[player's controls + 896] + 116] and r4 = the action, and answer with its
// result, but the object is gone from the registers when it returns.
thread_local uint32_t t_controls = 0;
thread_local uint32_t t_action = 0;
std::atomic<uint64_t> g_record_formula_mismatch{0};

bool RecordNativePoll(const char* native, PPCRegister& r3) {
  const uint32_t controls = t_controls;
  const uint32_t action = t_action;
  t_controls = 0;
  const int32_t latch_ms = REXCVAR_GET(mcla_script_pad_latch_ms);
  const bool probe = Probe();
  if ((!probe && !LatchWanted()) || !controls) {
    return false;
  }
  const bool real = (r3.u32 & 0xFF) != 0;
  if (probe && real != RecordPressed(controls, action) &&
      g_record_formula_mismatch.fetch_add(1, std::memory_order_relaxed) < 20) {
    REXLOG_INFO("TS record formula differs from the game: native={} controls={:08X} action={} game says {}",
                native, controls, action, real ? 1 : 0);
  }
  const ScriptSite s = CurrentScriptSite();
  const uint64_t src = SrcRecord(controls, action);
  const PollOut out = ScriptPoll(s.key, src, native, s.program, s.pc, real, latch_ms, controls, action);
  if (probe && out.first) {
    TraceSiteNew(native, s, src);
  }
  return out.latched;
}
}  // namespace

// 0x8220782C in sub_822077F0 (UILogic_IsPressed(player, action)) and 0x822A6DC8 in sub_822A6D48
// (Player_FlashingHighbeams(player): action 9): the `bl sub_822A4CE0`; r3 = the controls object,
// r4 = the action.
void mcla_script_ui_pressed_pre(PPCRegister& r3, PPCRegister& r4) {
  t_controls = r3.u32;
  t_action = r4.u32;
}

void mcla_script_highbeams_pre(PPCRegister& r3, PPCRegister& r4) {
  t_controls = r3.u32;
  t_action = r4.u32;
}

// 0x82207830 and 0x822A6DCC: after the call; r3 = the answer (the hook may set it to 1).
void mcla_script_ui_pressed_post(PPCRegister& r3) {
  if (RecordNativePoll("UILogic_IsPressed", r3)) {
    r3.u64 = 1;
  }
}

void mcla_script_highbeams_post(PPCRegister& r3) {
  if (RecordNativePoll("Player_FlashingHighbeams", r3)) {
    r3.u64 = 1;
  }
}

// 0x82557A68: the script loader sub_82557A68(r3 = the name to show, r4 = the script name). The
// programs' name hash (the word at +4 of the program) is the hash of r4 (sub_82557F78).
namespace {
uint32_t ScriptNameHash(const std::string& name) {
  uint32_t h = 0;
  size_t i = 0;
  const bool quoted = !name.empty() && name[0] == '"';
  if (quoted) {
    i++;
  }
  for (; i < name.size(); i++) {
    int c = static_cast<signed char>(name[i]);
    if (quoted && c == '"') {
      break;
    }
    if (c >= 'A' && c <= 'Z') {
      c += 32;
    } else if (c == '\\') {
      c = '/';
    }
    uint32_t v = uint32_t(c) + h;
    v += v << 10;
    h = (v >> 6) ^ v;
  }
  h += h << 3;
  h ^= h >> 11;
  h += h << 15;
  return h < 2 ? h + 2 : h;
}

std::string GuestString(uint32_t address, size_t max = 120) {
  std::string out;
  if (address < 0x10000) {
    return out;
  }
  const uint8_t* p = rex::Runtime::instance()->virtual_membase() + address;
  for (size_t i = 0; i < max && p[i]; i++) {
    out += (p[i] >= 32 && p[i] < 127) ? char(p[i]) : '?';
  }
  return out;
}
}  // namespace

void mcla_script_load(PPCRegister& r3, PPCRegister& r4) {
  if (!Probe()) {
    return;
  }
  const std::string name = GuestString(r4.u32);
  REXLOG_INFO("TS script load name=\"{}\" shown=\"{}\" hash={:08X} t={} update={}", name,
              GuestString(r3.u32), ScriptNameHash(name), NowUs(),
              g_update.load(std::memory_order_relaxed));
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

// The main thread's wait for a free slot in the game's command ring, without polling.
//
// The game keeps a ring of three slots per context at 0x82836814: a word at +0 that the main
// thread advances when it finishes writing a slot, and one at +4 that a guest thread sets when
// the slot has been used (sub_82160088 is one place that does: the `stw r11,4(r6)` at
// 0x82160140). Before the main thread writes into the next slot it waits for the ring not to be
// full (sub_82148A10): for up to 1,000 microseconds on the engine's microsecond clock it calls
// sub_821C91B8(0), which is Sleep(0) (a SwitchToThread), reads the word at +4 and the clock, and
// goes round again. In the city that loop is 2.8 of the main thread's 6.4 ms of CPU a frame
// (cpu-profile, launcher/dev/profiling-2026-10-26.md), 30% of it in the kernel.
//
// With mcla_poll_wait the loop's Sleep(0) is replaced by a wait that gives the core up: a spin on
// the word with the processor's pause hint for mcla_poll_spin_us, then a sleep on a
// high-resolution timer (and an event that the code that frees a slot sets) until
// mcla_poll_tail_us before the loop's deadline, then a spin again for the rest. The tail is there
// because the timer fires 0.3 ms late on average. The loop is otherwise the game's own: it reads
// the word and the clock after every wait, so a wake-up that never comes costs nothing but the
// sleep's length, and the game's own give-up after the 1,000 microseconds is where it was.
//
// Who sets the word (mcla_poll_find_writer, 2026-10-27): the only store to a word at +4 is the one
// in sub_82160088, which sub_82144888 calls once a round; that loop is the guest thread that
// sleeps between rounds, and it comes round about 144 times a second, once a frame. The word at
// +0 is stored by the main thread itself at the end of sub_82161210, so there is nothing to hook
// there. The release below therefore covers every wait that can end on a change of the word; a
// wait ends that way only when a round falls within its 1,000 microseconds, and in the city four
// in five do not (they end at the deadline).
//
// The same loop ends each round with a Sleep (see mcla_poll_tick and mcla_pump_sleep_max_ms).
//
// Wake on demand (mcla_pump_wake_on_demand): that Sleep is what holds the uncapped game near 145 frames a
// second, because one round of the loop frees exactly one slot, so the main thread, once its own work takes
// less than the 7 ms of the loop's rhythm, waits for the next round. The Sleep becomes a wait on an event
// with the same timeout (mcla_poll_tick skips the game's call); the main thread's ring wait (mcla_poll_wait, the
// hook is in the game's loop whether or not the option of that name is on) sets the event when it finds the
// ring full and the thread is waiting, so the round comes at once. While the main thread has no need of the
// thread (a frame takes it longer than the rhythm, or the frame rate is capped below it) nothing sets the
// event and the thread keeps its 7 ms. The audio watchdog turns it off for the rest of the session when the
// audio output runs dry while the thread was coming round early (the SDK's steady underrun counter).

#include "generated/default/mcla_init.h"

#include "handoff_trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <rex/audio/audio_dump.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(mcla_poll_wait, false, "MCLA",
                    "The main thread waits for a free slot of the game's command ring by sleeping on "
                    "a timer (woken by the code that frees a slot) between a short spin and a spin "
                    "before the loop's deadline, instead of calling Sleep(0) in a loop: about 2.8 ms "
                    "of the 6.4 ms of CPU a frame the main thread uses in the city, a third of it in "
                    "the kernel. Off is the game's own loop");
REXCVAR_DEFINE_INT32(mcla_poll_spin_us, 40, "MCLA",
                     "With mcla_poll_wait: microseconds the main thread spins on the slot's word "
                     "(with the pause hint) before it sleeps")
    .range(0, 2000);
REXCVAR_DEFINE_INT32(mcla_poll_tail_us, 400, "MCLA",
                     "With mcla_poll_wait: microseconds before the loop's deadline at which the main "
                     "thread stops sleeping and spins, the high-resolution timer being late by 0.3 ms "
                     "on average")
    .range(0, 1000);
REXCVAR_DEFINE_INT32(mcla_pump_sleep_max_ms, -1, "MCLA",
                     "The guest thread that frees the command ring's slots (the loop of sub_82144888) ends "
                     "each round with a Sleep of what is left of 7 ms, in whole milliseconds (4 to 5 ms in the "
                     "city), which keeps the frame rate near 143 a second whenever the rest of the frame is "
                     "faster. -1 leaves the game's own Sleep; 0 or more caps its argument at that many "
                     "milliseconds (0 is a Sleep(0)). Takes effect at once; for experiments: with 4 logical "
                     "processors it starved the audio worker (504 to 859 underruns), so it stays off")
    .range(-1, 16);
REXCVAR_DEFINE_BOOL(mcla_pump_wake_on_demand, false, "MCLA",
                    "The frame-pacing thread (the loop of sub_82144888, which frees the command ring's slots and "
                    "sleeps out 7 ms per round) waits on an event instead of sleeping, and the main thread sets the "
                    "event when it is waiting for a ring slot: the thread comes round at once when the main thread "
                    "has nothing else to do, and keeps its 7 ms rhythm otherwise. Lifts the 143 to 145 frames a "
                    "second that the uncapped game was held to on a CPU whose frame takes less than 7 ms; changes "
                    "nothing where the frame takes longer, or under a frame-rate cap below that. A steady audio "
                    "underrun while it runs turns it off for the rest of the session. Off is the game's own Sleep. "
                    "Has no effect while mcla_pump_sleep_max_ms is 0 or more");
REXCVAR_DEFINE_BOOL(mcla_pump_wake_early, false, "MCLA",
                    "With mcla_pump_wake_on_demand: the main thread also wakes the frame-pacing thread as soon as it has "
                    "published a slot of the command ring (the last statement of the game's world update), while that thread "
                    "is waiting and its last wait was ended early, so that the round that frees the slot runs while the main "
                    "thread does the rest of its frame and the slot is free when it asks for it: the wake-up and the round "
                    "are off its critical path. The thread's rounds still come once a frame; nothing changes under a frame-rate "
                    "cap or where the frame takes longer than its wait (its last wait then ran out). Experimental. Off is the "
                    "wake on demand alone");
REXCVAR_DEFINE_INT32(mcla_pump_wake_test_trip_s, 0, "MCLA",
                     "Development: with mcla_pump_wake_on_demand, this many seconds after the first early wake the "
                     "audio watchdog trips as if the audio had run dry (0 = never), to test the way back to the "
                     "game's own Sleep")
    .range(0, 3600);
REXCVAR_DEFINE_BOOL(mcla_poll_stats, false, "MCLA",
                    "Development: log how the main thread's waits for a ring slot ended, every 10 "
                    "seconds");
REXCVAR_DEFINE_BOOL(mcla_poll_find_writer, false, "MCLA",
                    "Development: make the page of the command ring read-only and log, every 10 "
                    "seconds, which guest functions write to it (the words at +0 and +4 of each "
                    "ring) and how often. Slow; for finding who frees a slot");

uint32_t MclaGuestFunctionForHostPc(uintptr_t host_pc);  // src/crash_trace.cpp

namespace {

uint8_t* GuestBase() {
  static uint8_t* base = [] {
    auto* runtime = rex::Runtime::instance();
    return runtime ? runtime->virtual_membase() : nullptr;
  }();
  return base;
}

struct Stats {
  std::atomic<uint64_t> calls{0}, done_in_spin{0}, slept{0}, woken_by_release{0}, woken_by_timer{0},
      tail_changed{0}, tail_deadline{0}, no_time_left{0}, no_sleep{0}, releases{0};
  // The thread that frees the slots (the loop of sub_82144888): how often it comes round, how far
  // apart its rounds are and what it passes to Sleep (whole milliseconds).
  std::atomic<uint64_t> ticks{0}, tick_periods{0}, tick_period_ns_sum{0}, tick_period_ns_max{0}, sleep_capped{0};
  std::atomic<uint64_t> sleep_ms[17]{};  // [16]: 16 or more
  std::atomic<int64_t> last_tick_ns{0};
  std::atomic<int64_t> last_log_ns{0};
};
Stats g_stats;

// mcla_pump_wake_on_demand.
struct Wake {
  std::atomic<bool> pump_waiting{false};  // the pump thread is inside its wait: the main thread may set the event
  std::atomic<bool> off{false};           // the audio watchdog tripped: the game's own Sleep from now on
  std::atomic<bool> set_recorded{false};  // the hand-off trace has the main thread's first set in this wait
  std::atomic<bool> last_early{false};    // the pump thread's last wait was ended early (by either wake)
  std::atomic<uint64_t> pre_signals{0};   // mcla_pump_wake_early: wake-ups at the publication of a slot
  std::atomic<uint64_t> waits{0}, early{0}, timed_out{0}, signals{0}, other_ring{0}, ring_calls{0};
  std::atomic<int64_t> first_early_ns{0}, last_early_ns{0};
  uint64_t steady_seen = 0;  // the pump thread's alone
  bool announced = false;    // the pump thread's alone
};
Wake g_wake;

float GuestFloat(uint32_t ea);
uint8_t GuestByte(uint32_t ea);
uint32_t GuestWord(uint32_t ea);
std::string GuestString(uint32_t ea, size_t max);

void MaybeLog() {
  using namespace std::chrono;
  const int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  int64_t last = g_stats.last_log_ns.load(std::memory_order_relaxed);
  if (last == 0) {
    g_stats.last_log_ns.compare_exchange_strong(last, now);
    return;
  }
  if (now - last < 10'000'000'000LL || !g_stats.last_log_ns.compare_exchange_strong(last, now)) {
    return;
  }
  REXLOG_INFO(
      "poll wait: {} calls: {} ended in the first spin, {} with no time left, {} without a sleep (too little time); "
      "{} slept ({} woken by the release, {} by the timer), then the tail spin ended {} on a change of the word, {} "
      "at the deadline; the word was set {} times",
      g_stats.calls.load(), g_stats.done_in_spin.load(), g_stats.no_time_left.load(), g_stats.no_sleep.load(),
      g_stats.slept.load(), g_stats.woken_by_release.load(), g_stats.woken_by_timer.load(),
      g_stats.tail_changed.load(), g_stats.tail_deadline.load(), g_stats.releases.load());
  if (g_stats.ticks.load()) {
    std::string histogram;
    for (int i = 0; i <= 16; ++i) {
      const uint64_t n = g_stats.sleep_ms[i].load();
      if (n) {
        histogram += std::string(" ") + (i == 16 ? ">=" : "") + std::to_string(i) + "ms:" + std::to_string(n);
      }
    }
    const uint64_t periods = g_stats.tick_periods.load();
    // 0x820036EC: the float that the loop takes its Sleep from (that, less the time the round took,
    // in whole milliseconds, kept between 0 and that); 0x827D1998: the byte that turns the Sleep on.
    REXLOG_INFO(
        "poll tick: the thread that sets the word came round {} times, {:.1f} us apart on average ({:.2f} ms at "
        "most), Sleep arguments (as the game worked them out):{}; the float at 0x820036EC is {}, the byte at "
        "0x827D1998 is {}; mcla_pump_sleep_max_ms {} capped {} of them",
        g_stats.ticks.load(), periods ? g_stats.tick_period_ns_sum.load() / 1000.0 / periods : 0.0,
        g_stats.tick_period_ns_max.load() / 1.0e6, histogram, GuestFloat(0x820036EC), GuestByte(0x827D1998),
        REXCVAR_GET(mcla_pump_sleep_max_ms), g_stats.sleep_capped.load());
    // What the loop calls each round: the function behind the pointer at 0x828366D0 (the loop's
    // object is at 0x82834D90, the call is `lwz r11,6464(r31)` / `bctrl`), and the name the
    // loop gives its timing scope (the string at 0x82003778).
    REXLOG_INFO("poll tick: the loop's callback is {:#010x}, its timing scope is \"{}\"", GuestWord(0x828366D0),
                GuestString(0x82003778, 48));
    if (REXCVAR_GET(mcla_pump_wake_on_demand)) {
      REXLOG_INFO(
          "poll tick: wake on demand{}: {} waits, {} ended early (the main thread set the event), {} ran out; the ring wait "
          "saw the thread waiting {} times, {} of them for another ring, and set the event {} times; {} wake-ups right after "
          "the main thread published a slot (mcla_pump_wake_early)",
          g_wake.off.load() ? " (OFF: the audio watchdog tripped)" : "", g_wake.waits.load(), g_wake.early.load(),
          g_wake.timed_out.load(), g_wake.ring_calls.load(), g_wake.other_ring.load(), g_wake.signals.load(),
          g_wake.pre_signals.load());
    }
  }
}

uint32_t Swap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

float GuestFloat(uint32_t ea) {
  uint8_t* base = GuestBase();
  if (!base) {
    return 0.0f;
  }
  const uint32_t raw = Swap32(*reinterpret_cast<const uint32_t*>(base + ea));
  float value;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

uint8_t GuestByte(uint32_t ea) {
  uint8_t* base = GuestBase();
  return base ? base[ea] : 0;
}

uint32_t GuestWord(uint32_t ea) {
  uint8_t* base = GuestBase();
  return base ? Swap32(*reinterpret_cast<const uint32_t*>(base + ea)) : 0;
}

std::string GuestString(uint32_t ea, size_t max) {
  uint8_t* base = GuestBase();
  std::string text;
  for (size_t i = 0; base && i < max && base[ea + i]; ++i) {
    const char c = static_cast<char>(base[ea + i]);
    text += (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  return text;
}

// ---- mcla_pump_wake_on_demand ---------------------------------------------------------------------------------------------
// The event the pump thread waits on in place of its Sleep, and what sets it.
#if defined(_WIN32)
HANDLE PumpWakeEvent() {
  static HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset
  return event;
}
bool PumpWakeUsable() {
  return PumpWakeEvent() != nullptr;
}
void PumpWakeReset() {
  ResetEvent(PumpWakeEvent());
}
void PumpWakeSet() {
  SetEvent(PumpWakeEvent());
}
bool PumpWakeWait(uint32_t ms) {  // true: the event was set; false: the time ran out
  return WaitForSingleObject(PumpWakeEvent(), ms) == WAIT_OBJECT_0;
}
#else
std::mutex g_pump_wake_mutex;
std::condition_variable g_pump_wake_cv;
bool g_pump_wake_is_set = false;
bool PumpWakeUsable() {
  return true;
}
void PumpWakeReset() {
  std::lock_guard<std::mutex> lock(g_pump_wake_mutex);
  g_pump_wake_is_set = false;
}
void PumpWakeSet() {
  {
    std::lock_guard<std::mutex> lock(g_pump_wake_mutex);
    g_pump_wake_is_set = true;
  }
  g_pump_wake_cv.notify_one();
}
bool PumpWakeWait(uint32_t ms) {
  std::unique_lock<std::mutex> lock(g_pump_wake_mutex);
  return g_pump_wake_cv.wait_for(lock, std::chrono::milliseconds(ms), [] { return g_pump_wake_is_set; });
}
#endif

int64_t SteadyNowNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// The main thread, in its ring wait (every turn of the loop of sub_82148A10): if the pump thread is waiting
// for its next round, tell it the main thread is waiting for it. The ring is picked by the byte at +199 of
// the context as the game's loop does; the pump thread frees the first one only.
void WakePumpIfWaiting(PPCRegister& r31) {
  if (!g_wake.pump_waiting.load(std::memory_order_acquire)) {
    return;
  }
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  const uint32_t ring = (uint32_t(base[r31.u32 + 199]) >> 2) & 0x3FFFFFF8u;
  g_wake.ring_calls.fetch_add(1, std::memory_order_relaxed);
  if (ring != 0) {
    g_wake.other_ring.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_wake.signals.fetch_add(1, std::memory_order_relaxed);
  if (!g_wake.set_recorded.load(std::memory_order_relaxed) && !g_wake.set_recorded.exchange(true, std::memory_order_relaxed)) {
    MclaHandoffEvent(mcla_handoff::kPumpWakeSet);
  }
  PumpWakeSet();
}

void TripWatchdog(const std::string& why) {
  if (g_wake.off.exchange(true)) {
    return;
  }
  REXLOG_WARN(
      "pump wake on demand: {}; the frame-pacing thread goes back to the game's own Sleep for the rest of this session "
      "({} early wakes, {} timeouts so far)",
      why, g_wake.early.load(), g_wake.timed_out.load());
}

// The audio watchdog (the pump thread, once a round): a silent frame in the middle of the game's audio, less than
// two seconds after a round that came early, turns the option off. A silence with no early wake in the last
// two seconds is not this option's doing (a stall, or a frame rate where it does not act) and is ignored.
void CheckAudio(int64_t now) {
  const uint64_t steady = rex::audio::dump::SteadyUnderruns();
  if (steady != g_wake.steady_seen) {
    const uint64_t added = steady - g_wake.steady_seen;
    g_wake.steady_seen = steady;
    const int64_t last = g_wake.last_early_ns.load(std::memory_order_relaxed);
    if (last != 0 && now - last < 2'000'000'000LL) {
      TripWatchdog("the audio output ran dry (" + std::to_string(added) +
                   " silent frames) while the thread was coming round early");
      return;
    }
  }
  const int32_t test = REXCVAR_GET(mcla_pump_wake_test_trip_s);
  const int64_t first = g_wake.first_early_ns.load(std::memory_order_relaxed);
  if (test > 0 && first != 0 && now - first >= static_cast<int64_t>(test) * 1'000'000'000LL) {
    TripWatchdog("mcla_pump_wake_test_trip_s of " + std::to_string(test) + " s");
  }
}

// The pump thread, in place of the game's Sleep(ms). Returns true when it waited (the hook then skips the game's
// call), false to leave the Sleep to the game (off, tripped, Sleep(0), no event).
bool PumpWait(uint32_t ms) {
  if (ms == 0 || g_wake.off.load(std::memory_order_relaxed) || !PumpWakeUsable()) {
    return false;
  }
  const int64_t now = SteadyNowNs();
  if (!g_wake.announced) {
    g_wake.announced = true;
    g_wake.steady_seen = rex::audio::dump::SteadyUnderruns();
    REXLOG_INFO(
        "pump wake on demand: on. The frame-pacing thread's Sleep is a wait that the main thread's ring wait ends early; "
        "a steady audio underrun while it comes round early turns it off");
  }
  CheckAudio(now);
  if (g_wake.off.load(std::memory_order_relaxed)) {
    return false;
  }
  // What the game's Sleep would wait: its argument in guest milliseconds, scaled like XThread::Delay does.
  const uint32_t wait_ms = rex::chrono::Clock::ScaleGuestDurationMillis(ms);
  if (wait_ms == 0) {
    return false;
  }
  PumpWakeReset();  // a set from an earlier wait is not this wait's
  g_wake.set_recorded.store(false, std::memory_order_relaxed);
  MclaHandoffEvent(mcla_handoff::kPumpSleepBegin, wait_ms);
  g_wake.pump_waiting.store(true, std::memory_order_seq_cst);
  const bool early = PumpWakeWait(wait_ms);
  g_wake.pump_waiting.store(false, std::memory_order_seq_cst);
  g_wake.last_early.store(early, std::memory_order_relaxed);
  MclaHandoffEvent(mcla_handoff::kPumpSleepEnd, early ? 1 : 0);
  g_wake.waits.fetch_add(1, std::memory_order_relaxed);
  if (early) {
    g_wake.early.fetch_add(1, std::memory_order_relaxed);
    const int64_t stamp = SteadyNowNs();
    int64_t zero = 0;
    g_wake.first_early_ns.compare_exchange_strong(zero, stamp, std::memory_order_relaxed);
    g_wake.last_early_ns.store(stamp, std::memory_order_relaxed);
  } else {
    g_wake.timed_out.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

// Rounds, Sleep arguments (as the game worked them out) and spacing of the freeing thread (mcla_poll_stats).
void CountTick(uint32_t asked) {
  const int64_t now = SteadyNowNs();
  const int64_t last = g_stats.last_tick_ns.exchange(now, std::memory_order_relaxed);
  if (last != 0) {
    const uint64_t period = static_cast<uint64_t>(now - last);
    g_stats.tick_periods.fetch_add(1, std::memory_order_relaxed);
    g_stats.tick_period_ns_sum.fetch_add(period, std::memory_order_relaxed);
    uint64_t seen = g_stats.tick_period_ns_max.load(std::memory_order_relaxed);
    while (period > seen && !g_stats.tick_period_ns_max.compare_exchange_weak(seen, period)) {
    }
  }
  g_stats.sleep_ms[std::min<uint32_t>(asked, 16)].fetch_add(1, std::memory_order_relaxed);
  MaybeLog();
}

#if defined(_WIN32)
// The event that the code freeing a slot sets, and one high-resolution timer per waiting thread.
HANDLE ReleaseEvent() {
  static HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset
  return event;
}

struct HrTimer {
  HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
  ~HrTimer() {
    if (handle) {
      CloseHandle(handle);
    }
  }
};

int64_t Qpc() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

int64_t QpcFrequency() {
  static const int64_t freq = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return v.QuadPart;
  }();
  return freq;
}

// ---- mcla_poll_find_writer ---------------------------------------------------------------------
// The page that holds the rings is made read-only; a write faults, is counted by the guest function
// it comes from (the dispatch table maps the host address back), and is let through with the trap
// flag, after which the page is protected again. Stores by other threads while one is stepping are
// missed, which is all right for finding the usual writer.
constexpr uint32_t kRingPage = 0x82836000;
constexpr uint32_t kRingWords = 0x82836810;  // the flag byte (set when a wait gives up), then the rings: 8 bytes each
constexpr uint32_t kRingWordsEnd = 0x82836834;

struct Writer {
  std::atomic<uint32_t> function{0};  // guest function, or 0 and `host` for code outside the game
  std::atomic<uint32_t> ea{0};
  std::atomic<uint64_t> count{0};
  std::atomic<uintptr_t> host{0};
};
Writer g_writers[48];
std::atomic<uint64_t> g_other_page_writes{0};
thread_local bool t_stepping = false;

void CountWriter(uint32_t function, uintptr_t host, uint32_t ea) {
  for (Writer& w : g_writers) {
    const uint32_t f = w.function.load(std::memory_order_relaxed);
    const uintptr_t h = w.host.load(std::memory_order_relaxed);
    if ((f == function && (function != 0 || h == host)) && w.ea.load(std::memory_order_relaxed) == ea) {
      w.count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (f == 0 && h == 0) {
      w.host.store(host, std::memory_order_relaxed);
      w.ea.store(ea, std::memory_order_relaxed);
      w.function.store(function, std::memory_order_relaxed);
      w.count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

LONG CALLBACK WriterVeh(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  uint8_t* base = GuestBase();
  if (!base) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  uint8_t* page = base + kRingPage;
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 &&
      record->ExceptionInformation[0] == 1) {
    uint8_t* fault = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
    if (fault < page || fault >= page + 0x1000) {
      return EXCEPTION_CONTINUE_SEARCH;
    }
    const uint32_t ea = static_cast<uint32_t>(fault - base);
    if (ea >= kRingWords && ea < kRingWordsEnd) {
      const uintptr_t pc = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
      CountWriter(MclaGuestFunctionForHostPc(pc), pc, ea);
    } else {
      g_other_page_writes.fetch_add(1, std::memory_order_relaxed);
    }
    DWORD old;
    VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
    info->ContextRecord->EFlags |= 0x100;  // trap flag: one instruction
    t_stepping = true;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_stepping) {
    t_stepping = false;
    DWORD old;
    VirtualProtect(page, 0x1000, PAGE_READONLY, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void InstallWriterTrace() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) {
    return;
  }
  uint8_t* base = GuestBase();
  if (!base) {
    return;
  }
  AddVectoredExceptionHandler(1, WriterVeh);
  DWORD old;
  VirtualProtect(base + kRingPage, 0x1000, PAGE_READONLY, &old);
  REXLOG_INFO("poll writer trace: the page at {:#010x} is read-only", kRingPage);
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      for (const Writer& w : g_writers) {
        const uint64_t n = w.count.load(std::memory_order_relaxed);
        if (n) {
          REXLOG_INFO("poll writer: {} writes to {:#010x} from guest function {:#010x} (host {:#x})", n,
                      w.ea.load(), w.function.load(), static_cast<uint64_t>(w.host.load()));
        }
      }
      REXLOG_INFO("poll writer: {} writes to the rest of the page", g_other_page_writes.load());
    }
  }).detach();
}
#endif

}  // namespace

// 0x82148AB0 in sub_82148A10: `li r3,0` ahead of `bl sub_821C91B8` (Sleep(0)), inside the wait for
// a free slot. r31 = the context (the byte at +199 picks the ring), r30 = 0x82836814 (the rings),
// r29 = the word at +4 as the loop last read it, r26 = the loop's deadline and r3 = its last clock
// reading (microseconds). True skips the Sleep(0).
bool mcla_poll_wait(PPCRegister& r31, PPCRegister& r30, PPCRegister& r29, PPCRegister& r26,
                    PPCRegister& r3) {
  WakePumpIfWaiting(r31);  // mcla_pump_wake_on_demand: the main thread is waiting for the thread that frees the slot
#if defined(_WIN32)
  if (REXCVAR_GET(mcla_poll_find_writer)) {
    InstallWriterTrace();
  }
  if (!REXCVAR_GET(mcla_poll_wait)) {
    return false;
  }
  uint8_t* base = GuestBase();
  static thread_local HrTimer timer;
  if (!base || !timer.handle) {
    return false;
  }
  // sub_82148A10's own index: rlwinm r9,r10,30,2,28 of the byte at +199.
  const uint32_t ring = (uint32_t(base[r31.u32 + 199]) >> 2) & 0x3FFFFFF8u;
  volatile uint32_t* word = reinterpret_cast<volatile uint32_t*>(base + r30.u32 + 4 + ring);
  const uint32_t expected = Swap32(r29.u32);  // guest memory is big-endian
  const bool stats = REXCVAR_GET(mcla_poll_stats);
  if (stats) {
    g_stats.calls.fetch_add(1, std::memory_order_relaxed);
    MaybeLog();
  }
  const int64_t freq = QpcFrequency();
  const int64_t entered = Qpc();
  // What the loop has left of its deadline, from the clock reading it has in r3.
  const int32_t left_us = static_cast<int32_t>(r26.u32 - r3.u32);
  if (left_us <= 0) {
    if (stats) {
      g_stats.no_time_left.fetch_add(1, std::memory_order_relaxed);
    }
    return true;  // the loop's next turn sees the deadline
  }
  const int64_t deadline = entered + freq * left_us / 1000000;
  // 1. A short spin: the word usually changes within it when a slot is nearly free.
  const int64_t spin_end = entered + freq * std::clamp(REXCVAR_GET(mcla_poll_spin_us), 0, 2000) / 1000000;
  int64_t now = entered;
  HANDLE release = ReleaseEvent();
  ResetEvent(release);  // before the word is read again: a release after the read sets it
  for (;;) {
    for (int i = 0; i < 16; ++i) {
      if (*word != expected) {
        if (stats) {
          g_stats.done_in_spin.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
      }
      YieldProcessor();
    }
    now = Qpc();
    if (now >= spin_end || now >= deadline) {
      break;
    }
  }
  // 2. Sleep until the tail before the deadline, or until the word is released.
  const int64_t tail = freq * std::clamp(REXCVAR_GET(mcla_poll_tail_us), 0, 1000) / 1000000;
  const int64_t sleep_ticks = deadline - tail - now;
  if (sleep_ticks > freq / 5000) {  // at least 200 us
    LARGE_INTEGER due;
    due.QuadPart = -(sleep_ticks * 10'000'000 / freq);  // 100 ns units, relative
    if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0)) {
      const HANDLE handles[2] = {release, timer.handle};
      const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
      if (stats) {
        g_stats.slept.fetch_add(1, std::memory_order_relaxed);
        (woke == WAIT_OBJECT_0 ? g_stats.woken_by_release : g_stats.woken_by_timer)
            .fetch_add(1, std::memory_order_relaxed);
      }
      if (woke == WAIT_OBJECT_0) {
        CancelWaitableTimer(timer.handle);
        return true;  // a slot was released: the loop reads the word
      }
    }
  } else if (stats) {
    g_stats.no_sleep.fetch_add(1, std::memory_order_relaxed);
  }
  // 3. The tail: spin until the word changes or the deadline.
  for (;;) {
    for (int i = 0; i < 16; ++i) {
      if (*word != expected) {
        if (stats) {
          g_stats.tail_changed.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
      }
      YieldProcessor();
    }
    if (Qpc() >= deadline) {
      break;
    }
  }
  if (stats) {
    g_stats.tail_deadline.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
#else
  (void)r31, (void)r30, (void)r29, (void)r26, (void)r3;
  return false;
#endif
}

// 0x82160140 in sub_82160088, after `stw r11,4(r6)`: r6 = 0x82836814, so the word at r6+4 has just
// been set; wake the main thread if it sleeps. This is the only store to a word at +4 in the game
// (mcla_poll_find_writer found no other), so every wait that can end by a change of the word
// ends here.
void mcla_poll_release(PPCRegister& r6) {
  (void)r6;
#if defined(_WIN32)
  if (REXCVAR_GET(mcla_poll_stats)) {
    g_stats.releases.fetch_add(1, std::memory_order_relaxed);
  }
  if (REXCVAR_GET(mcla_poll_wait)) {
    SetEvent(ReleaseEvent());
  }
#endif
}

// 0x821449C8 in sub_82144888, after `lwz r3,84(r1)`: the loop of the thread that calls
// sub_82160088 (the one that sets the word) has the milliseconds it is about to pass to Sleep in
// r3 (sub_821C91B8: what is left of the float at 0x820036EC, 7, after the round's own time,
// truncated to whole milliseconds, and only while the byte at 0x827D1998 is set). With
// mcla_pump_sleep_max_ms of 0 or more the argument is capped there: nothing else reads r3, the
// Sleep is the next call. With mcla_poll_stats it counts the rounds, the Sleep arguments (as the
// game worked them out) and the time between rounds, which is how often a slot is freed.
// With mcla_pump_wake_on_demand the Sleep is done here, as a wait on an event that the main thread's ring
// wait sets (PumpWait), and the return value true skips the game's call: the hook's jump_address_on_true
// is the instruction after it (nothing reads the Sleep's result; the loop starts its next round).
bool mcla_poll_tick(PPCRegister& r3) {
  const uint32_t asked = r3.u32;
  const int32_t cap = REXCVAR_GET(mcla_pump_sleep_max_ms);
  if (cap >= 0 && asked > static_cast<uint32_t>(cap)) {
    r3.u64 = static_cast<uint32_t>(cap);
    g_stats.sleep_capped.fetch_add(1, std::memory_order_relaxed);
  }
  g_stats.ticks.fetch_add(1, std::memory_order_relaxed);  // the dev pad's `state` reports it
  if (REXCVAR_GET(mcla_poll_stats)) {
    CountTick(asked);
  }
  // With the cap set the experiment is the game's Sleep with a smaller argument, not this wait.
  return cap < 0 && REXCVAR_GET(mcla_pump_wake_on_demand) && PumpWait(asked);
}

// The main thread, right after it publishes a slot of the ring: the last statement of the game's world update (sub_82161210,
// the hook of config/handoff_trace.toml), which makes the ring full for it until the pump thread frees a slot. With
// mcla_pump_wake_early the pump thread, if it is waiting and its last wait was ended early (the regime where the main thread
// waits for it: the frame is shorter than its rhythm), is woken now instead of when the main thread asks, so that the round
// runs beside the rest of the main thread's frame. Ring 0 only, the ring that thread frees.
void MclaPumpPreWake(uint32_t ring_offset) {
  if (ring_offset != 0 || !REXCVAR_GET(mcla_pump_wake_early) || !g_wake.pump_waiting.load(std::memory_order_acquire) ||
      !g_wake.last_early.load(std::memory_order_relaxed)) {
    return;
  }
  g_wake.pre_signals.fetch_add(1, std::memory_order_relaxed);
  if (!g_wake.set_recorded.load(std::memory_order_relaxed) && !g_wake.set_recorded.exchange(true, std::memory_order_relaxed)) {
    MclaHandoffEvent(mcla_handoff::kPumpWakeSet, 1);
  }
  PumpWakeSet();
}

// Rounds the freeing thread has made since the start (src/dev_pad.cpp's `state`).
uint64_t MclaPumpRounds() {
  return g_stats.ticks.load(std::memory_order_relaxed);
}

// Rounds that the freeing thread's wait ended early because the main thread set the event (src/dev_pad.cpp's `state`).
uint64_t MclaPumpEarlyWakes() {
  return g_wake.early.load(std::memory_order_relaxed);
}

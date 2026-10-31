// The game's audio mixer thread: a host priority for it, and its round in the audio statistics.
//
// How the game's audio reaches the speakers (read from the recompiled code, fork issue #21): the audio worker
// (the SDK's "Audio Worker" thread) runs the game's callback (sub_823F56D0) once per 5.33 ms frame. The callback
// does almost nothing itself (about 7 us): its helper sub_823ECA40 sets an event (0x828CD9AC, at 0x823ECA90) and
// waits for another (0x828CD99C). The thread that answers is a guest thread whose body is sub_823EC8C8: it waits
// for the first event, mixes the frame (sub_823EC160, sub_823EC248; about 720 us of CPU on a fast PC) and sets the
// second at 0x823EC978. The game asks for priority +15 for it (KeSetBasePriorityThread), which the SDK ignores
// (ignore_thread_priorities), so on a PC with few cores it runs at the same priority as the render, GPU and job
// threads, and when they take the cores it waits to run: with 4 busy processes on 4 cores the audio worker's round
// went from 0.74 to 3.8 ms and the output ran dry.
//
// audio_mixer_priority gives that thread the treatment audio_thread_priority gives the audio worker and the XMA
// decoder thread (1 above normal, 2 highest, 3 highest and the Windows multimedia scheduler's "Pro Audio" task). The
// thread is found by its entry address, 0x823EC8C8: the override of sub_823EC8C8 below runs on it when it starts. Its
// handle (F8000534 in most runs, F8000530 in one of 11) is not a way to find it; the entry was the same in all 11.
// The SDK's patch (rexglue-xma-decoder-fixes.patch) does the host calls and knows nothing of this game.
// Windows only; elsewhere the option does nothing. The game turns it on by itself with 5 or fewer logical processors
// (mcla_small_cpu_defaults, src/mcla_app.cpp), where audio_thread_priority is raised too.
//
// With audio_stats the audio statistics line also carries the mixer's round: from its wake-up (0x823EC920, just
// after its wait returns) to its signal to the pump (0x823EC978): the wall time (busy), its CPU time (a busy that is
// much longer than its CPU time is a thread that waits to run, or on something that is not a CPU), and the delay
// from the pump's signal (0x823ECA90, just before its call) to the wake-up. The hooks are in config/audio_mixer.toml;
// without audio_stats each costs one relaxed load.
// (launcher/dev/audio-mixer-2026-10-30.md)

#include "generated/default/mcla_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/audio/audio_dump.h>
#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(audio_mixer_priority, 0, "MCLA",
                     "Host priority of the game's own audio mixer thread (the guest thread that mixes each 5.33 ms "
                     "frame for the audio worker; its priority request is ignored): 0 = normal, 1 = above normal, "
                     "2 = highest, 3 = highest and registered with the Windows multimedia scheduler (MMCSS, \"Pro "
                     "Audio\"), as audio_thread_priority does for the audio worker. With 5 or fewer logical "
                     "processors the game sets 3 (mcla_small_cpu_defaults; a value you set wins). Windows only; "
                     "nothing elsewhere. Takes effect at the next start")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The thread's body: the generated code, called by the override below.
REX_EXTERN(__imp__sub_823EC8C8);

#ifdef MCLA_LAB_FMA
// Lab build (never merged): the calling thread's fma call counts (src/fma_fast.cpp).
uint64_t MclaFmaThreadCalls();
uint64_t MclaFmaThreadEligible();
bool MclaFmaCounting();
#endif

namespace {

constexpr uint32_t kMixerEntry = 0x823EC8C8;
// The audio rounds after which the mixer thread must have started (about 0.27 s of audio): the pump cannot go round
// without it, so a thread that has not started by then is not the one this file knows.
constexpr uint64_t kRoundsToStart = 50;

std::atomic<uint32_t> g_mixer_starts{0};  // threads that started with the mixer's entry
std::atomic<uint64_t> g_pump_rounds{0};   // the pump's signals to the mixer

// The mixer's round, for the statistics (audio_stats only). The times are steady-clock nanoseconds.
struct Totals {
  std::atomic<uint64_t> rounds{0}, busy_ns{0}, cpu_ns{0}, wake_n{0}, wake_ns{0};
  std::atomic<uint64_t> busy_max_ns{0}, wake_max_ns{0};  // since the last interval line
  std::atomic<uint64_t> fma_calls{0}, fma_eligible{0};     // lab: the fma calls of the mixer's rounds
};
Totals g_totals;
std::atomic<uint64_t> g_pump_signal_ns{0};  // when the pump last signalled the mixer; 0 = taken by the wake-up

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void RaiseMax(std::atomic<uint64_t>& a, uint64_t v) {
  uint64_t cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

struct Snap {
  uint64_t rounds = 0, busy_ns = 0, cpu_ns = 0, wake_n = 0, wake_ns = 0, fma_calls = 0, fma_eligible = 0;
};

Snap Take() {
  Snap s;
  s.rounds = g_totals.rounds.load(std::memory_order_relaxed);
  s.busy_ns = g_totals.busy_ns.load(std::memory_order_relaxed);
  s.cpu_ns = g_totals.cpu_ns.load(std::memory_order_relaxed);
  s.wake_n = g_totals.wake_n.load(std::memory_order_relaxed);
  s.wake_ns = g_totals.wake_ns.load(std::memory_order_relaxed);
  s.fma_calls = g_totals.fma_calls.load(std::memory_order_relaxed);
  s.fma_eligible = g_totals.fma_eligible.load(std::memory_order_relaxed);
  return s;
}

// The end of the audio statistics lines (rex::audio::dump::SetExtraStats): the mixer's rounds since the last interval
// line (total false, called by the reporter thread only) or since the start.
std::string MixerStats(bool total) {
  static Snap prev;                                  // the reporter thread's, for the interval lines
  static std::atomic<uint64_t> run_busy_max_ns{0};  // the maxima of the intervals so far
  static std::atomic<uint64_t> run_wake_max_ns{0};
  const Snap now = Take();
  const Snap before = total ? Snap{} : prev;
  uint64_t busy_max = 0, wake_max = 0;
  if (total) {
    busy_max = std::max(run_busy_max_ns.load(std::memory_order_relaxed), g_totals.busy_max_ns.load(std::memory_order_relaxed));
    wake_max = std::max(run_wake_max_ns.load(std::memory_order_relaxed), g_totals.wake_max_ns.load(std::memory_order_relaxed));
  } else {
    busy_max = g_totals.busy_max_ns.exchange(0, std::memory_order_relaxed);
    wake_max = g_totals.wake_max_ns.exchange(0, std::memory_order_relaxed);
    RaiseMax(run_busy_max_ns, busy_max);
    RaiseMax(run_wake_max_ns, wake_max);
    prev = now;
  }
  const uint64_t rounds = now.rounds - before.rounds;
  if (rounds == 0) {
    return " | mixer: rounds=0";
  }
  const uint64_t wake_n = now.wake_n - before.wake_n;
  char cpu[24] = "na";
  if (rex::audio::dump::ThreadCpuAvailable()) {
    std::snprintf(cpu, sizeof(cpu), "%llu", static_cast<unsigned long long>((now.cpu_ns - before.cpu_ns) / 1000 / rounds));
  }
  char buf[384];
  int n = std::snprintf(buf, sizeof(buf),
                " | mixer: rounds=%llu busy_avg_us=%llu busy_max_us=%llu cpu_avg_us=%s wake_avg_us=%llu wake_max_us=%llu",
                static_cast<unsigned long long>(rounds),
                static_cast<unsigned long long>((now.busy_ns - before.busy_ns) / 1000 / rounds),
                static_cast<unsigned long long>(busy_max / 1000), cpu,
                static_cast<unsigned long long>(wake_n ? (now.wake_ns - before.wake_ns) / 1000 / wake_n : 0),
                static_cast<unsigned long long>(wake_max / 1000));
#ifdef MCLA_LAB_FMA
  if (MclaFmaCounting() && n > 0 && n < static_cast<int>(sizeof(buf))) {
    std::snprintf(buf + n, sizeof(buf) - n, " fma_per_round=%llu fma_eligible_per_round=%llu",
                  static_cast<unsigned long long>((now.fma_calls - before.fma_calls) / rounds),
                  static_cast<unsigned long long>((now.fma_eligible - before.fma_eligible) / rounds));
  }
#else
  (void)n;
#endif
  return buf;
}

[[maybe_unused]] const bool g_stats_registered = [] {
  rex::audio::dump::SetExtraStats(&MixerStats);
  return true;
}();

// The wake-up and the end of the current round, per thread (the mixer is one thread; thread_local keeps it honest).
thread_local uint64_t t_round_start_ns = 0;
thread_local uint64_t t_round_start_cpu_ns = 0;
thread_local uint64_t t_round_start_fma = 0;
thread_local uint64_t t_round_start_fma_eligible = 0;

}  // namespace

// The mixer thread's body (the entry of the guest thread the game creates with flags 0x10000001 and 256 KiB of stack, in
// the same millisecond as it registers its audio callback). Runs on that thread.
extern "C" REX_FUNC(sub_823EC8C8) {
  g_mixer_starts.fetch_add(1, std::memory_order_relaxed);
  const int32_t level = REXCVAR_GET(audio_mixer_priority);
  if (level > 0) {
#if defined(_WIN32)
    rex::audio::RaiseCallingAudioThread(level, "audio mixer thread (guest entry 823EC8C8)");
#else
    REXLOG_WARN("audio_mixer_priority {} has no effect on this platform", level);
#endif
  }
  __imp__sub_823EC8C8(ctx, base);
}

// 0x823ECA90 in sub_823ECA40, the `bl KeSetEvent` of the event the mixer waits on: the pump is about to wake the
// mixer. Also the check that the mixer thread has started: the pump cannot go on without it.
void mcla_audio_pump_signal() {
  const uint64_t round = g_pump_rounds.fetch_add(1, std::memory_order_relaxed) + 1;
  if (round == kRoundsToStart && g_mixer_starts.load(std::memory_order_relaxed) == 0) {
    const int32_t level = REXCVAR_GET(audio_mixer_priority);
    if (level > 0) {
      REXLOG_WARN(
          "audio mixer thread: no guest thread with entry {:08X} had started when the audio pump had run {} rounds; "
          "audio_mixer_priority {} was not applied",
          kMixerEntry, kRoundsToStart, level);
    }
  }
  if (rex::audio::dump::StatsEnabled()) {
    g_pump_signal_ns.store(NowNs(), std::memory_order_relaxed);
  }
}

// 0x823EC920 in sub_823EC8C8: its KeWaitForSingleObject has returned; a round begins.
void mcla_audio_mixer_woke() {
  if (!rex::audio::dump::StatsEnabled()) {
    return;
  }
  const uint64_t now = NowNs();
  const uint64_t signalled = g_pump_signal_ns.exchange(0, std::memory_order_relaxed);
  if (signalled != 0 && now >= signalled) {
    const uint64_t wake = now - signalled;
    g_totals.wake_n.fetch_add(1, std::memory_order_relaxed);
    g_totals.wake_ns.fetch_add(wake, std::memory_order_relaxed);
    RaiseMax(g_totals.wake_max_ns, wake);
  }
  t_round_start_ns = now;
  t_round_start_cpu_ns = rex::audio::dump::ThreadCpuNs();
#ifdef MCLA_LAB_FMA
  t_round_start_fma = MclaFmaThreadCalls();
  t_round_start_fma_eligible = MclaFmaThreadEligible();
#endif
}

// 0x823EC978 in sub_823EC8C8: the `bl KeSetEvent` that tells the pump the frame is mixed; the round ends.
void mcla_audio_mixer_done() {
  if (!rex::audio::dump::StatsEnabled() || t_round_start_ns == 0) {
    return;
  }
  const uint64_t busy = NowNs() - t_round_start_ns;
  const uint64_t cpu_now = rex::audio::dump::ThreadCpuNs();
  g_totals.rounds.fetch_add(1, std::memory_order_relaxed);
  g_totals.busy_ns.fetch_add(busy, std::memory_order_relaxed);
  if (cpu_now >= t_round_start_cpu_ns) {
    g_totals.cpu_ns.fetch_add(cpu_now - t_round_start_cpu_ns, std::memory_order_relaxed);
  }
  RaiseMax(g_totals.busy_max_ns, busy);
#ifdef MCLA_LAB_FMA
  g_totals.fma_calls.fetch_add(MclaFmaThreadCalls() - t_round_start_fma, std::memory_order_relaxed);
  g_totals.fma_eligible.fetch_add(MclaFmaThreadEligible() - t_round_start_fma_eligible, std::memory_order_relaxed);
#endif
  t_round_start_ns = 0;
}

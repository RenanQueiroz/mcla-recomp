// Frame pacing and frame-rate targets.
//
// The game was built for a locked 30 FPS: its timer replaces the measured
// frame time with a fixed 33.3 ms step, so any frame that runs long plays in
// slow motion, and with vsync a 35 ms frame waits until 50 ms. Here the timer
// is given the real elapsed time instead, vsync is turned off, and a host
// limiter sleeps to a wall-clock deadline. A late frame then costs only its
// overrun, and the simulation advances by real time at any frame rate.
//
// Hook addresses and the timer analysis come from LARecomp (see
// config/frame_timing.toml).

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/low_latency.h>
#include <rex/ui/presenter.h>
#include <rex/runtime.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#include <immintrin.h>
#endif

REXCVAR_DEFINE_INT32(mcla_fps, 30, "MCLA",
                     "Frame-rate target: 30, 60 or 120. 0 runs uncapped. "
                     "-1 restores the game's original fixed 30 Hz timing")
    .range(-1, 240);

REXCVAR_DEFINE_BOOL(mcla_timer_hr_at_cap, true, "MCLA",
                    "Windows: while a frame-rate cap is set, the timer thread waits on a high-resolution timer "
                    "(timer_queue_hr_wait) even where the game leaves that off (6 or more logical processors), "
                    "which gives back the core it keeps busy: about half of the process's CPU at a 60 FPS cap. "
                    "Without a cap the value the game started with applies. A timer_queue_hr_wait the player has "
                    "set is never changed. Off leaves timer_queue_hr_wait alone (the game's automatic value by "
                    "processor count, with or without a cap)");

// The cap while frame generation is on (the SDK's frame_generation): the rendered
// rate, so 60 shows 120. -1 is HALF DISPLAY, just under half the display's refresh
// rate (floor of its 97%, so 116 on 240 Hz and 58 on 120 Hz: the presents then
// stay under the display's own limit and the swap to screen stays short). While
// frame generation is off, or can't run, mcla_fps applies, as it does with HALF
// DISPLAY on a display under 100 Hz, where half would be under 50 rendered.
REXCVAR_DEFINE_INT32(mcla_fps_frame_generation, -1, "MCLA",
                     "Frame-rate cap with frame generation on: -1 is HALF DISPLAY (just under half "
                     "the display's refresh rate), 0 uncapped, or the rendered rate. mcla_fps "
                     "applies while it is off")
    .range(-1, 240);

// Off by default: skipping here also skips whatever puts the game logo on the
// title screen (only "PRESS START" remains). Tested with and without the
// render-pass mask change; the logo is missing either way.
REXCVAR_DEFINE_BOOL(mcla_skip_intro, false, "MCLA",
                    "Skip the intro movies. Side effect: the title screen loses its logo")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The legal screens and logos at start-up. "skip" is mcla_skip_intro by
// another name, so that the settings menu can offer all three in one row.
REXCVAR_DEFINE_STRING(mcla_intro, "normal", "MCLA",
                      "Start-up legal screens and logos: normal (original speed), fast (as "
                      "fast as the host presents), skip (the title screen loses its logo)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Where the frame limiter waits. At the start of the frame (the engine timer),
// the simulation step is measured before the wait while the frame reaches the
// screen after a variable amount of work, so at the cap moving objects advance
// unevenly against their display times and the picture shakes. Holding the
// finished frame at its swap instead is what vsync does, with a precise clock:
// frames are presented at even intervals and each step measures one interval.
REXCVAR_DEFINE_BOOL(mcla_pace_at_swap, true, "MCLA",
                    "Hold frames to the FPS target at presentation, like a precise vsync, "
                    "instead of before the game's update");

// HALF DISPLAY (mcla_fps_frame_generation = -1): floor(97% of half the refresh rate) rendered,
// so the presents (twice as many) stay just under what the display takes; 0 where the
// refresh rate isn't known or is under 100 Hz, where half is under 50 and the
// player's own cap stays.
int32_t MclaHalfDisplayCap(float refresh_hz) {
  if (!(refresh_hz >= 100.0f)) {
    return 0;
  }
  return int32_t(std::floor(double(refresh_hz) * 0.97 / 2.0));
}

namespace {

// The engine timer object lives at a fixed guest address (sub_821BDA90).
constexpr uint32_t kGuestFrameDelta = 0x827D7508;  // seconds, time-scaled
constexpr uint32_t kGuestFrameRate = 0x827D750C;   // 1 / delta

// Longest time one frame may advance the game clock. A streaming stall must
// not reach physics and audio as one huge step.
constexpr double kMaxFrameSeconds = 0.125;

// Set when the frame limiter has run; see PaceUnlimitedSwap.
std::atomic<bool> g_limiter_ran_since_swap{false};
// Set by the first pass through the engine timer: the start-up logos are over.
std::atomic<bool> g_engine_timer_started{false};

bool OriginalTiming() {
  return REXCVAR_GET(mcla_fps) < 0;
}

// The frame-rate cap now: mcla_fps, or with frame generation on mcla_fps_frame_generation
// (HALF DISPLAY from the display's refresh rate, read about once a second).
// Frame generation counts as on while the SDK's frame_generation asks for it and
// has not said it can't run (Presenter::GetFrameGenerationStatus).
uint64_t NowMicros();

int32_t CapFromSettings() {
  const int32_t fps = REXCVAR_GET(mcla_fps);
  if (fps < 0 || rex::cvar::GetFlagByName("frame_generation") == "off" ||
      rex::ui::Presenter::GetFrameGenerationStatus().state ==
          rex::ui::Presenter::FrameGenerationState::kUnavailable) {
    return fps;
  }
  const int32_t cap = REXCVAR_GET(mcla_fps_frame_generation);
  if (cap >= 0) {
    return cap;
  }
  // The display's refresh rate comes from the presenter's display reader, which runs
  // after a window change or when asked: asked again every two seconds, for a window
  // moved to another display or a changed mode. Not known yet (the first time here:
  // a moment's wait at most, once), or never (not read on every platform), the
  // player's cap stays.
  static std::atomic<uint64_t> asked_us{0};
  float refresh = rex::ui::Presenter::GetDisplayRefreshHz();
  const uint64_t now = NowMicros();
  const uint64_t asked = asked_us.load(std::memory_order_relaxed);
  if (asked == 0 || now - asked >= 2'000'000) {
    asked_us.store(now, std::memory_order_relaxed);
    rex::ui::Presenter::RefreshHdrDisplayInfo(asked == 0 && !(refresh > 0.0f) ? 100 : 0);
    refresh = rex::ui::Presenter::GetDisplayRefreshHz();
  }
  const int32_t half = MclaHalfDisplayCap(refresh);
  return half > 0 ? half : fps;
}

void TimerQueueFollowsCap(int32_t fps);

uint64_t NowMicros() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// Sleeps until the next frame deadline. The deadline advances by one period
// per frame; a frame that is already late resets it, so lateness is not
// repaid by running the following frames faster. Each call site keeps its own
// deadline (the template parameter).
enum PaceSite { kPaceAtTimer, kPaceAtSwap };

// mcla_timer_hr_at_cap: the timer thread's wait on a high-resolution timer (the SDK's timer_queue_hr_wait) while a frame-rate cap is
// set. The wait gives back the core the timer thread keeps busy otherwise (a whole core at a 30 or 60 FPS cap on 6 or more logical
// processors, where the game leaves it off by default); what it costs is a frame that is 0.4 to 1.7% longer when the game runs
// uncapped near 145 FPS, which a cap hides. The value the game started with (the automatic one by processor count, or the player's)
// is what applies without a cap; a value the player has set is never changed.
void TimerQueueFollowsCap(int32_t fps) {
#if defined(_WIN32)
  if (!REXCVAR_GET(mcla_timer_hr_at_cap)) {
    return;
  }
  constexpr const char* kName = "timer_queue_hr_wait";
  static bool base_known = false;
  static bool base_on = false;
  if (rex::cvar::GetFlagSource(kName) != rex::cvar::Source::kDefault) {
    return;  // set by the player (the command line, mcla.toml, the overlay): theirs
  }
  if (!base_known) {
    base_known = true;
    base_on = rex::cvar::GetFlagByName(kName) == "true";
  }
  const bool want = fps > 0 || base_on;
  if ((rex::cvar::GetFlagByName(kName) == "true") != want && rex::cvar::SetDefaultValue(kName, want ? "true" : "false")) {
    REXLOG_INFO("frame timing: timer queue high-resolution wait {} ({})", want ? "on" : "off",
                fps > 0 ? "a frame-rate cap is set" : "no cap");
  }
#else
  (void)fps;
#endif
}

// CapFromSettings, read at most every 250 ms: the frame limiter runs once or twice a
// frame.
int32_t EffectiveFps() {
  static std::atomic<int32_t> cap{0};
  static std::atomic<uint64_t> checked_us{0};
  const uint64_t now = NowMicros();
  const uint64_t last = checked_us.load(std::memory_order_relaxed);
  if (last == 0 || now - last >= 250'000) {
    checked_us.store(now, std::memory_order_relaxed);
    const int32_t value = CapFromSettings();
    if (cap.exchange(value, std::memory_order_relaxed) != value) {
      REXLOG_INFO("frame timing: the frame-rate cap is now {} FPS{}", value,
                  value == 0 ? " (uncapped)" : "");
      TimerQueueFollowsCap(value);
    }
    return value;
  }
  return cap.load(std::memory_order_relaxed);
}

template <PaceSite kSite>
void WaitForFrameDeadline() {
  static uint64_t next_us = 0;
  // The timer function has callers on other threads; pace only the first one.
  // There is one swap per frame, from whichever thread presents.
  static const std::thread::id owner = std::this_thread::get_id();
  if (kSite == kPaceAtTimer && std::this_thread::get_id() != owner) {
    return;
  }

  // The driver's limiter holds the frame rate (low_latency_limiter = driver): never twice.
  if (rex::ui::LowLatency::DriverLimits()) {
    next_us = 0;
    return;
  }

  const int32_t fps = EffectiveFps();
  if (fps <= 0) {
    next_us = 0;
    return;
  }
  const uint64_t period_us = 1'000'000ull / static_cast<uint64_t>(fps);

  uint64_t now = NowMicros();
  if (next_us == 0) {
    next_us = now + period_us;
    return;
  }
  if (now < next_us) {
    // Sleep most of the way, then spin the last stretch: sleeps overshoot by
    // up to a millisecond or two, which is a large share of an 8.3 ms frame.
    constexpr uint64_t kSpinMicros = 1500;
    if (next_us - now > kSpinMicros + 500) {
      std::this_thread::sleep_for(std::chrono::microseconds(next_us - now - kSpinMicros));
    }
    while (NowMicros() < next_us) {
#if defined(_WIN32)
      _mm_pause();
#else
      std::this_thread::yield();
#endif
    }
  }
  now = NowMicros();
  next_us += period_us;
  if (next_us < now) {
    next_us = now + period_us;
  }
}

float ReadGuestFloat(uint32_t address) {
  const uint8_t* p = rex::Runtime::instance()->virtual_membase() + address;
  uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  float value;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

}  // namespace

void MclaTickButtonPrompts();
void MclaTickPerfOptions();
// The dev pad (src/dev_pad.cpp), a PC development tool the PS5 build doesn't
// compile: no-ops there.
#if REX_PLATFORM_PS5
static void MclaDevPadFrame(uint64_t&) {}
static void MclaDevPadSwap() {}
#else
void MclaDevPadFrame(uint64_t& ticks);
void MclaDevPadSwap();
#endif
void MclaApplyMsaaAtSwap();
void MclaTickSettingsMenu();
// src/sampling_profiler.cpp: the frame counts of its windows.
void MclaProfileCountSwap();
void MclaProfileCountUpdate();
#ifdef _WIN32
// src/mcla_app.cpp: whether the GPU plugin loaded is Vulkan's.
bool MclaGraphicsIsVulkan();
#endif

// Reflex sleep assist. With NVIDIA Reflex holding the frame-rate cap on Direct3D 12, the driver's
// wait before each frame (NvAPI_D3D_Sleep, inside LowLatency::OnFrameStart) is a busy wait: the
// profile of the update thread shows the performance counter, the driver's own frames and short
// sleeps (ZwDelayExecution) for the whole wait, 6.4 to 6.9 ms of CPU a frame at a cap of 60 FPS
// and about 15 ms at 30 (cpu3 and handoff analyses). On Vulkan the same wait is a semaphore
// (vkLatencySleepNV) and costs no CPU, so this is for Direct3D 12 only.
//
// With mcla_reflex_sleep_assist (on by default since v0.3.0) the update thread first waits on a
// high-resolution waitable timer until the margin before the moment the driver's wait would end,
// and then calls the driver as before, which spins only the last stretch.
//
// The margin. mcla_reflex_sleep_margin_us is a fixed part (1000 to 8000 us, default 1500).
// mcla_reflex_sleep_margin_pct (default 20 since v0.3.0) adds a share of the cap's frame
// interval. The margin is the larger of the two (the default: 3.3 ms at 60 FPS, 6.7 ms at 30,
// 1.7 ms at 120), or their sum with mcla_reflex_sleep_margin_sum (1000 us plus 17 percent is the
// rule of DXVK's latency sleep, which sleeps and then spins its last 4 timer granularities plus a
// sixth of the wait: about 3.8 ms at 60 FPS and 6.6 ms at 30). The first measurement (fixed
// 1.5 ms, without the frame-cap timer) cut the process's CPU at the caps by 19 to 44 percent and
// paced a little less evenly. The sweep of launcher/dev/reflex-assist-margin-2026-10-28.md (on
// top of the frame-cap timer) found 20 percent within the noise of the assist's absence for the
// pacing at 4 and 16 logical processors and caps of 60 and 30, at the same CPU saving (26 to 51
// percent less per swap). The check below widens whatever margin the settings give.
//
// The deadline. The documentation says only that the minimum frame interval (NV_SET_SLEEP_MODE_
// PARAMS::minimumIntervalUs, set by the low latency code to 1,000,000 / the cap) is enforced
// and that NvAPI_D3D_Sleep is where the driver sleeps for it and for the latency reduction
// (the Vulkan text for the same field says: the interval between presents of a swapchain).
// It does not say which two events the interval is measured between, and the latency
// reduction can move the end of the wait. So the deadline is a model, checked on every frame
// it is used for:
//   - the model is the previous start plus the interval: the time the previous call of
//     LowLatency::OnFrameStart returned plus 1,000,000 / the cap (the same integer division the
//     driver was told). In steady state the starts are one interval apart, so the model is
//     exact when the driver measures start to start, and off by how much the work of one frame
//     differs from the driver's estimate of it when it measures from the presents;
//   - it is used only while the driver holds the cap (LowLatency::DriverLimits(): the mode was
//     taken, an interval is set, and the last wait was made), never without a cap (the driver's
//     own pacing then has no known deadline: not engaged), never on Vulkan, never with
//     low_latency_limiter = fork (the game's own limiter holds the cap), never with frame
//     generation (the game's own limiter holds it too), and only after 16 passes in a row at
//     the same cap;
//   - the timer wait ends the margin before the model's deadline, so the model may be late by
//     the margin: that also covers the timer's lateness (0.3 ms on average);
//   - the check: after the timer, the driver's call is timed. If it returned at once (under 200
//     us) although the model said it had at least 700 us to wait, the driver's deadline is
//     earlier than the model's and the timer may have delayed the frame's start: the margin is
//     widened by 0.5 ms (up to 4 ms) and narrows again by 0.125 ms after 64 frames without it.
//     8 troubles (that or a timer that woke over 1 ms late) among the last 32 frames switch
//     the assist off for 10 s, and it starts again with the model checked from the beginning.
// A frame whose work already took longer than the interval minus the margin makes no timer wait.
//
// It moves where the wait is spent (the timer instead of the driver's spin) and nothing the game
// computes: the engine timer still reads the time after the wait, and the simulation step is the
// elapsed time as before. The one change to a frame's timing is a start later than the driver's
// choice, by at most how much the widened margin was short: seen as "too tight" in the log.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

REXCVAR_DEFINE_BOOL(mcla_reflex_sleep_assist, true, "MCLA",
                    "With NVIDIA Reflex holding the frame-rate cap on Direct3D 12 (Windows): the "
                    "update thread waits on a high-resolution timer until the margin (the larger of "
                    "mcla_reflex_sleep_margin_us and mcla_reflex_sleep_margin_pct percent of the "
                    "cap's frame interval) before the moment the driver's wait would end, so the "
                    "driver's busy wait covers only the last stretch (6.4 to 6.9 ms of CPU a frame "
                    "at a cap of 60 FPS otherwise). Timing only. false is the driver's own wait");
REXCVAR_DEFINE_INT32(mcla_reflex_sleep_margin_us, 1500, "MCLA",
                     "With mcla_reflex_sleep_assist: microseconds before the driver's expected "
                     "deadline at which the timer wait ends and the driver takes over. It covers "
                     "the timer's lateness (0.3 ms on average) and how far the driver's own "
                     "deadline is from the model; widened by itself when the driver's wait after "
                     "the timer comes back at once. With mcla_reflex_sleep_margin_pct above 0 it "
                     "is the fixed part of the margin")
    .range(1000, 8000);
REXCVAR_DEFINE_DOUBLE(mcla_reflex_sleep_margin_pct, 20.0, "MCLA",
                      "With mcla_reflex_sleep_assist: the share, in percent, of the cap's frame "
                      "interval that is left to the driver's own wait (20: 6.7 ms at a cap of 30 FPS, "
                      "3.3 ms at 60). 0 is off (only "
                      "mcla_reflex_sleep_margin_us). The margin is the larger of this share and "
                      "mcla_reflex_sleep_margin_us, or their sum with mcla_reflex_sleep_margin_sum. "
                      "A larger margin takes less CPU off the driver's wait")
    .range(0.0, 80.0);
REXCVAR_DEFINE_BOOL(mcla_reflex_sleep_margin_sum, false, "MCLA",
                    "With mcla_reflex_sleep_margin_pct above 0: the margin is "
                    "mcla_reflex_sleep_margin_us plus the share instead of the larger of the two "
                    "(1000 and 17 is DXVK's rule: about 3.8 ms at 60 FPS, 6.6 ms at 30)");

#ifdef _WIN32
namespace {

class ReflexSleepAssist {
 public:
  // On the frame thread, in the frame-start hook, before LowLatency::OnFrameStart. `fps` is the
  // cap now (0: uncapped), which is the one the driver is told in this pass.
  void Before(int32_t fps) {
    assisted_ = false;
    enabled_ = REXCVAR_GET(mcla_reflex_sleep_assist);
    if (!enabled_) {
      Reset();
      return;
    }
    const uint64_t now = NowMicros();
    const char* why = Gate(fps, now);
    Note(why, now);
    if (why) {
      streak_ = 0;
      engaged_ = false;
      return;
    }
    if (fps != fps_) {
      fps_ = fps;
      streak_ = 0;
    }
    if (streak_ < kWarmPasses) {
      ++streak_;
      return;
    }
    const uint64_t period = 1'000'000ull / uint64_t(fps);
    const uint64_t margin = Margin(period);
    if (!engaged_) {
      engaged_ = true;
      stats_ = Stats();
      stats_since_us_ = now;
      REXLOG_INFO(
          "Reflex sleep assist: on at {} FPS (Direct3D 12): a timer wait until {} us before the "
          "driver's deadline (the previous start plus {} us; margin rule: {}), then NVIDIA "
          "Reflex's own wait",
          fps, margin, period, MarginRule(period));
    }
    const uint64_t deadline = prev_return_us_ + period;
    if (deadline <= now + margin + kMinWaitUs) {
      ++stats_.idle;  // the frame's work used the time already: the driver waits what is left
      MaybeReport(fps, now);
      return;
    }
    const uint64_t wake = deadline - margin;
    const uint64_t wait_us = std::min<uint64_t>(wake - now, period);
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(wait_us * 10);  // 100 ns units, relative
    if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer_, static_cast<DWORD>(wait_us / 1000 + 4));
    } else {
      ++stats_.timer_errors;
      return;
    }
    call_us_ = NowMicros();
    wake_us_ = wake;
    deadline_us_ = deadline;
    assisted_ = true;
    stats_.timer_wait_us += call_us_ - now;
    MaybeReport(fps, call_us_);
  }

  // Right after LowLatency::OnFrameStart returned.
  void After() {
    if (!enabled_) {
      return;
    }
    const uint64_t returned = NowMicros();
    if (assisted_) {
      const int64_t driver_us = int64_t(returned) - int64_t(call_us_);
      const int64_t expected_us = int64_t(deadline_us_) - int64_t(call_us_);
      const int64_t late_us = int64_t(call_us_) - int64_t(wake_us_);
      const int64_t against_model = int64_t(returned) - int64_t(deadline_us_);
      ++stats_.assisted;
      stats_.driver_wait_us += uint64_t(std::max<int64_t>(driver_us, 0));
      stats_.model_error_sum += against_model;
      stats_.model_error_min = std::min(stats_.model_error_min, against_model);
      stats_.model_error_max = std::max(stats_.model_error_max, against_model);
      bool trouble = false;
      if (late_us > kLateWakeUs) {
        trouble = true;
        ++stats_.late;
      } else if (expected_us >= kExpectedUs && driver_us < kImmediateUs) {
        // The driver had nothing to wait for although the model said it had: its deadline is
        // earlier than the model's, and the timer may have held the frame back.
        trouble = true;
        ++stats_.tight;
        extra_us_ = std::min<int64_t>(extra_us_ + kWidenUs, kMaxExtraUs);
        calm_ = 0;
      } else if (++calm_ >= 64) {
        calm_ = 0;
        extra_us_ = std::max<int64_t>(extra_us_ - kNarrowUs, 0);
      }
      trouble_history_ = (trouble_history_ << 1) | (trouble ? 1u : 0u);
      if (CountBits(trouble_history_) >= 8) {
        holdoff_until_us_ = returned + kHoldOffUs;
        REXLOG_WARN(
            "Reflex sleep assist: the driver's waits do not fit the model (8 of the last 32 "
            "frames were too tight or woke late; since the last report {} too tight and {} late "
            "wake-ups, margin widened by {} us); off for 10 s, then checked from the start",
            stats_.tight, stats_.late, extra_us_);
        trouble_history_ = 0;
        extra_us_ = 0;
        engaged_ = false;
        streak_ = 0;
      }
    }
    prev_return_us_ = returned;
  }

 private:
  static constexpr uint32_t kWarmPasses = 16;       // passes at one cap before the first timer wait
  static constexpr uint64_t kMinWaitUs = 500;       // no timer wait for less than this
  static constexpr int64_t kImmediateUs = 200;      // the driver's call returned at once
  static constexpr int64_t kExpectedUs = 700;       // ... although the model gave it this long
  static constexpr int64_t kLateWakeUs = 1000;      // the timer woke this late
  static constexpr int64_t kWidenUs = 500;
  static constexpr int64_t kNarrowUs = 125;
  static constexpr int64_t kMaxExtraUs = 4000;
  static constexpr uint64_t kHoldOffUs = 10'000'000;
  static constexpr uint64_t kReportUs = 5'000'000;

  struct Stats {
    uint64_t assisted = 0;        // passes with a timer wait
    uint64_t idle = 0;            // passes with nothing to wait for
    uint64_t tight = 0;           // the driver's call came back at once after the timer
    uint64_t late = 0;            // the timer woke over 1 ms late
    uint64_t timer_errors = 0;
    uint64_t timer_wait_us = 0;   // total
    uint64_t driver_wait_us = 0;  // total, the driver's call after the timer
    int64_t model_error_sum = 0;  // the return against the model's deadline
    int64_t model_error_min = INT64_MAX;
    int64_t model_error_max = INT64_MIN;
  };

  static uint32_t CountBits(uint32_t v) {
    uint32_t n = 0;
    for (; v; v &= v - 1) {
      ++n;
    }
    return n;
  }

  // The margin the settings give for a frame interval of period_us, before the check's widening:
  // the fixed part, the share of the interval, and the larger of the two or their sum. The cvars
  // are read every pass, so a change takes effect at once.
  static uint64_t FixedMargin() {
    return uint64_t(std::clamp<int64_t>(REXCVAR_GET(mcla_reflex_sleep_margin_us), 1000, 8000));
  }
  static double SharePercent() {
    return std::clamp<double>(REXCVAR_GET(mcla_reflex_sleep_margin_pct), 0.0, 80.0);
  }
  static uint64_t BaseMargin(uint64_t period_us) {
    const uint64_t fixed = FixedMargin();
    const uint64_t share = uint64_t(double(period_us) * SharePercent() / 100.0 + 0.5);
    return REXCVAR_GET(mcla_reflex_sleep_margin_sum) ? fixed + share : std::max(fixed, share);
  }
  uint64_t Margin(uint64_t period_us) const { return BaseMargin(period_us) + uint64_t(extra_us_); }

  // The rule in words, for the log.
  static std::string MarginRule(uint64_t period_us) {
    char buf[160];
    if (SharePercent() <= 0.0) {
      std::snprintf(buf, sizeof(buf), "%llu us fixed", (unsigned long long)FixedMargin());
    } else {
      std::snprintf(buf, sizeof(buf),
                    REXCVAR_GET(mcla_reflex_sleep_margin_sum) ? "%llu us plus %.1f%% of %llu us"
                                                              : "the larger of %llu us and %.1f%% of %llu us",
                    (unsigned long long)FixedMargin(), SharePercent(), (unsigned long long)period_us);
    }
    return buf;
  }

  void Reset() {
    prev_return_us_ = 0;
    streak_ = 0;
    engaged_ = false;
    extra_us_ = 0;
    calm_ = 0;
    trouble_history_ = 0;
    pending_why_.clear();
    logged_why_.clear();
  }

  // Null when the assist may run; else why not. Text starting with '(' is a state not worth a line.
  const char* Gate(int32_t fps, uint64_t now) {
    if (MclaGraphicsIsVulkan()) {
      return "Vulkan: its wait before the frame is a semaphore, not a spin (not applicable)";
    }
    if (now < holdoff_until_us_) {
      return "(off for a while: the driver's waits did not fit the model)";
    }
    if (!rex::ui::LowLatency::DriverLimits()) {
      return WhyDriverDoesNotHold(fps, now);
    }
    if (fps <= 0) {
      return "no frame-rate cap: the driver's own pacing has no known deadline";
    }
    const uint64_t period = 1'000'000ull / uint64_t(fps);
    const uint64_t base = BaseMargin(period);
    if (period < base + 3000) {
      return "the cap's interval is too short for the margin";
    }
    if (period < base + uint64_t(extra_us_) + 3000) {
      // The margin widened so far that nothing is left to wait: start again from the base.
      holdoff_until_us_ = now + kHoldOffUs;
      extra_us_ = 0;
      return "(off for a while: the margin had to be widened past the cap's interval)";
    }
    if (!EnsureTimer()) {
      return "no high-resolution waitable timer (Windows 10 1803 or later)";
    }
    if (!prev_return_us_) {
      return "(first pass)";
    }
    return nullptr;
  }

  // Checked at most every 2 s: it takes the low latency lock and copies strings.
  const char* WhyDriverDoesNotHold(int32_t fps, uint64_t now) {
    if (!diagnosis_.empty() && now - diagnosed_us_ < 2'000'000) {
      return diagnosis_.c_str();
    }
    diagnosed_us_ = now;
    const auto status = rex::ui::LowLatency::GetStatus();
    if (status.state == rex::ui::LowLatency::State::kUnknown) {
      diagnosis_ = "(no latency reducer yet)";
    } else if (status.state == rex::ui::LowLatency::State::kUnavailable) {
      diagnosis_ = "no latency reducer: " + status.name + " is unavailable (" + status.why + ")";
    } else if (status.name != "NVIDIA REFLEX") {
      diagnosis_ = status.name + " is not NVIDIA Reflex (not applicable, untested)";
    } else if (!status.active) {
      diagnosis_ = "NVIDIA Reflex is off or given up (low_latency, or the wait took too long)";
    } else if (rex::cvar::GetFlagByName("low_latency_limiter") == "fork") {
      diagnosis_ = "low_latency_limiter is fork: the game's own limiter holds the cap";
    } else if (rex::cvar::GetFlagByName("frame_generation") != "off" &&
               rex::ui::Presenter::GetFrameGenerationStatus().state !=
                   rex::ui::Presenter::FrameGenerationState::kUnavailable) {
      diagnosis_ = "frame generation is on: the game's own limiter holds the cap";
    } else if (fps <= 0) {
      diagnosis_ = "no frame-rate cap: the driver's own pacing has no known deadline";
    } else {
      diagnosis_ = "(the driver does not hold the cap yet)";
    }
    return diagnosis_.c_str();
  }

  // One line per reason, once it has lasted 3 s (the start-up and the loading screens pass through
  // several); the same reason is logged again after the assist has run in between.
  void Note(const char* why, uint64_t now) {
    if (!why) {
      pending_why_.clear();
      logged_why_.clear();
      return;
    }
    if (why[0] == '(' || logged_why_ == why) {
      return;
    }
    if (pending_why_ != why) {
      pending_why_ = why;
      pending_since_us_ = now;
      return;
    }
    if (now - pending_since_us_ >= 3'000'000) {
      logged_why_ = why;
      REXLOG_INFO("Reflex sleep assist: not running: {}", why);
    }
  }

  void MaybeReport(int32_t fps, uint64_t now) {
    if (now - stats_since_us_ < kReportUs) {
      return;
    }
    const Stats& s = stats_;
    if (s.assisted) {
      const uint64_t period = 1'000'000ull / uint64_t(std::max<int32_t>(fps, 1));
      REXLOG_INFO(
          "Reflex sleep assist: {} timer waits and {} frames with nothing to wait for in {:.1f} s at "
          "{} FPS: timer wait {:.0f} us on average, the driver's wait after it {:.0f} us, its "
          "return against the model (previous start plus interval) {:+.0f} us on average (from "
          "{:+d} to {:+d}), margin {} us (widened by {}), {} too tight, {} late wake-ups, {} timer "
          "errors",
          s.assisted, s.idle, double(now - stats_since_us_) / 1e6, fps,
          double(s.timer_wait_us) / double(s.assisted), double(s.driver_wait_us) / double(s.assisted),
          double(s.model_error_sum) / double(s.assisted), s.model_error_min, s.model_error_max,
          Margin(period), extra_us_, s.tight, s.late, s.timer_errors);
    }
    stats_ = Stats();
    stats_since_us_ = now;
  }

  bool EnsureTimer() {
    if (!timer_ && !timer_failed_) {
      timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
      if (!timer_) {
        timer_failed_ = true;
        REXLOG_WARN(
            "Reflex sleep assist: no high-resolution waitable timer (error {}); the driver does "
            "the whole wait",
            GetLastError());
      }
    }
    return timer_ != nullptr;
  }

  HANDLE timer_ = nullptr;
  bool timer_failed_ = false;
  uint64_t prev_return_us_ = 0;  // when the previous LowLatency::OnFrameStart returned
  int32_t fps_ = -1;
  uint32_t streak_ = 0;          // passes in a row the assist could run at this cap
  bool engaged_ = false;
  int64_t extra_us_ = 0;         // the margin widened by what the check found
  uint32_t calm_ = 0;
  uint32_t trouble_history_ = 0;
  uint64_t holdoff_until_us_ = 0;
  // The pass in progress.
  bool enabled_ = false;
  bool assisted_ = false;
  uint64_t call_us_ = 0;
  uint64_t wake_us_ = 0;
  uint64_t deadline_us_ = 0;
  Stats stats_;
  uint64_t stats_since_us_ = 0;
  // The log.
  std::string diagnosis_;
  uint64_t diagnosed_us_ = 0;
  std::string pending_why_;
  uint64_t pending_since_us_ = 0;
  std::string logged_why_;
};

}  // namespace
#endif

// Low latency (NVIDIA Reflex, AMD Anti-Lag 2): the frame starts at the top of the engine timer,
// on the update thread, before the timer reads the time: the driver's wait comes before the
// time is read (the simulation step then measures the frames' starts, after the wait, and not
// before it) and before anything reads the input. The timer function has callers on other
// threads; the first one is the frame's.
//
// The hook sits after the timer's first label, which the timer's own retry jumps back to when the
// low half of the time it read was 0 (about once in 2^32 reads): that pass of the hook, a few
// instructions after the first, is the same frame and does nothing (no frame is 100 us long).
void mcla_frame_start(PPCRegister& r3) {
  static const std::thread::id frame_thread = std::this_thread::get_id();
  static uint64_t last_start_us = 0;  // the frame thread's
  if (std::this_thread::get_id() == frame_thread && !OriginalTiming()) {
    if (NowMicros() - last_start_us < 100) {
      return;
    }
    const int32_t fps = EffectiveFps();
    rex::ui::LowLatency::SetFrameLimit(fps > 0 ? uint32_t(fps) : 0);
#ifdef _WIN32
    // Reflex's busy wait on Direct3D 12 is cut to its last stretch by a timer wait first (above).
    static ReflexSleepAssist sleep_assist;
    sleep_assist.Before(fps);
#endif
    rex::ui::LowLatency::OnFrameStart();
#ifdef _WIN32
    sleep_assist.After();
#endif
    last_start_us = NowMicros();  // after the driver's wait
  }
}

void mcla_frame_delta(PPCRegister& r8) {
  MclaProfileCountUpdate();
  MclaTickButtonPrompts();
  MclaTickPerfOptions();
  MclaTickSettingsMenu();
  MclaDevPadFrame(r8.u64);
  if (OriginalTiming()) {
    return;
  }
  if (!REXCVAR_GET(mcla_pace_at_swap)) {
    WaitForFrameDeadline<kPaceAtTimer>();
  }
  g_limiter_ran_since_swap.store(true, std::memory_order_relaxed);
  g_engine_timer_started.store(true, std::memory_order_relaxed);
  // r8 was computed before the wait. Whatever the wait added is picked up by
  // the next frame's measurement, so no time is lost, only shifted one frame.
  static const uint64_t max_ticks = static_cast<uint64_t>(
      kMaxFrameSeconds * static_cast<double>(rex::chrono::Clock::guest_tick_frequency()));
  if (r8.u64 > max_ticks) {
    r8.u64 = max_ticks;
  }
}

bool mcla_use_real_delta() {
  return !OriginalTiming();
}

void mcla_fixed_step_path(PPCRegister& r3, PPCRegister& f11) {
  if (OriginalTiming()) {
    return;
  }
  f11.f64 = static_cast<double>(ReadGuestFloat(r3.u32 + 0x58));
}

// Loops that never reach the engine timer (the publisher logos at start-up,
// loading screens) were paced on the console only by the present interval of
// two vblanks, which is removed here, so they ran as fast as the host could
// present: the logo sequence was over in about a second. Such a loop shows as
// swaps with no pass through the frame limiter between them; from the second
// of those in a row, swaps are held to the original 30 per second.
//
// A single one is not enough. The game updates and presents on different
// threads, and when uncapped the two drift against each other: about one swap
// in forty has no update before it and the next one has two. Holding those to
// 33 ms each made uncapped play stutter (and, by delaying the presenting
// thread, made the pattern ten times as frequent: a quarter of all frames).
//
// The fast and skip intros leave such loops unpaced only until the engine
// timer first runs, which is the start-up logos; loading screens after that
// keep the 30.

static void PaceUnlimitedSwap(bool timed) {
  static std::atomic<uint64_t> last_swap_us{0};
  static std::atomic<bool> last_untimed{false};
  constexpr uint64_t kPeriodMicros = 1'000'000ull / 30;
  const bool untimed_loop = !timed && last_untimed.exchange(true, std::memory_order_relaxed);
  if (timed) {
    last_untimed.store(false, std::memory_order_relaxed);
  }
  const bool fast_logos = REXCVAR_GET(mcla_intro) != "normal" &&
                          !g_engine_timer_started.load(std::memory_order_relaxed);
  const uint64_t last = last_swap_us.load(std::memory_order_relaxed);
  uint64_t now = NowMicros();
  if (untimed_loop && !fast_logos && last && now - last < kPeriodMicros) {
    std::this_thread::sleep_for(std::chrono::microseconds(kPeriodMicros - (now - last)));
    now = NowMicros();
  }
  last_swap_us.store(now, std::memory_order_relaxed);
}

bool mcla_present_interval(PPCRegister& r11) {
  MclaProfileCountSwap();
  MclaDevPadSwap();
  MclaApplyMsaaAtSwap();
  if (OriginalTiming()) {
    return false;
  }
  // A swap that follows a pass through the engine timer is a gameplay frame:
  // with mcla_pace_at_swap it is held here to the FPS target.
  const bool timed = g_limiter_ran_since_swap.exchange(false, std::memory_order_relaxed);
  if (timed && REXCVAR_GET(mcla_pace_at_swap)) {
    WaitForFrameDeadline<kPaceAtSwap>();
  }
  PaceUnlimitedSwap(timed);
  r11.u64 = 1;
  return true;
}

// A filter written as "move a fraction k toward the target every frame" is
// only right at the frame rate it was tuned for. The same filter over an
// arbitrary step dt is 1 - (1 - k)^(dt * 30), where k is the 30 FPS fraction.
static double PerFrameToContinuous(double k30, double dt) {
  return 1.0 - std::pow(1.0 - k30, dt * 30.0);
}

static void RescaleCameraFactor(PPCRegister& reg) {
  if (OriginalTiming()) {
    return;
  }
  const double dt = ReadGuestFloat(kGuestFrameDelta);
  const double raw = reg.f64;
  if (raw <= 0.0 || raw >= 1.0 || dt <= 0.0) {
    return;
  }
  // The game halves the tune value itself when its integer frame rate is
  // below 60 (0x82320454..0x82320464), so the value arriving here is already
  // the 30 FPS factor in that case and the raw tune value otherwise.
  const float fps = ReadGuestFloat(kGuestFrameRate);
  const bool already_halved = static_cast<int>(fps + 0.5f) < 60;
  reg.f64 = PerFrameToContinuous(already_halved ? raw : 0.5 * raw, dt);
}

void mcla_camera_pos_smoothing(PPCRegister& f13) {
  RescaleCameraFactor(f13);
}

void mcla_camera_lookat_smoothing(PPCRegister& f0) {
  RescaleCameraFactor(f0);
}

void mcla_chassis_depth_smoothing(PPCRegister& f0) {
  if (OriginalTiming()) {
    return;
  }
  const double dt = ReadGuestFloat(kGuestFrameDelta);
  if (dt > 0.0) {
    f0.f64 = PerFrameToContinuous(0.10, dt);
  }
}

// The motorcycle's gyro (mcBikeGyro::Update) eases a smoothed position and
// velocity toward the bike's by a fixed fraction per frame: its tune values
// (0.40 and 0.15) from a rounded frame rate of 60 up, doubled below that into
// the console's 30 FPS fractions. It matches the console only at 30. At 120
// the position settles about 1.3 and the velocity 1.8 times as fast, at 45
// both 1.5 times, and a 60 FPS cap whose frames straddle 59.5 FPS switches
// from frame to frame between a looser filter and one two to three times as
// stiff.
REXCVAR_DEFINE_BOOL(mcla_bike_gyro_fix, true, "MCLA",
                    "Ease the motorcycle's lean by elapsed time, as on the console at 30 FPS, "
                    "instead of by a fixed fraction per frame");

namespace {
// The game's multiplier below 60 FPS (2.0).
constexpr uint32_t kBikeGyroLowRateScale = 0x82040E94;
}  // namespace

static void RescaleBikeGyroFactor(PPCRegister& reg) {
  if (OriginalTiming() || !REXCVAR_GET(mcla_bike_gyro_fix)) {
    return;
  }
  const double dt = ReadGuestFloat(kGuestFrameDelta);
  const double raw = reg.f64;
  if (raw <= 0.0 || raw >= 1.0 || dt <= 0.0) {
    return;
  }
  // Below 60 the value arriving here has already been doubled (0x82368958,
  // 0x823689EC), after the same rounded frame-rate test as the camera's.
  const float fps = ReadGuestFloat(kGuestFrameRate);
  const bool already_doubled = static_cast<int>(fps + 0.5f) < 60;
  const double k30 = already_doubled ? raw : raw * ReadGuestFloat(kBikeGyroLowRateScale);
  if (k30 <= 0.0 || k30 >= 1.0) {
    return;
  }
  reg.f64 = PerFrameToContinuous(k30, dt);
}

void mcla_bike_gyro_pos_smoothing(PPCRegister& f13) {
  RescaleBikeGyroFactor(f13);
}

void mcla_bike_gyro_vel_smoothing(PPCRegister& f0) {
  RescaleBikeGyroFactor(f0);
}

// The UI's Flash movies (menus, pop-ups, and the HUD with its song ticker and
// "LOADING" badge) advance exactly one movie frame per game update: the UI
// manager steps them in the Flash player's one-frame mode, which ignores the
// elapsed time. The console updated 30 times a second, which is what the
// movies were made for; at 120 FPS they all ran four times as fast (the song
// ticker under the speedometer scrolls 2 px per update at 720p: 60 px/s at 30,
// 240 at 120). Each movie is now advanced on its own schedule, 30 a second
// (the HUD at a rate of its own, below), and an update that comes before a
// movie is due skips that movie's advance.
// A movie due within a quarter of a frame is advanced early, so at 30 FPS every
// update still advances once, as before. A movie that falls behind (below 30
// FPS) is not caught up, as on the console. The start-up logos are left alone:
// the fast intro is meant to run them as fast as possible.
//
// The in-game HUD is two movies: HUDMAPMOVIE and the gauge movie, which the
// HUD code creates itself, so it is not among the UI manager's screens. The
// game sets the gauges' values every update and the movies' own scripts draw
// them as they advance, in the same step that scrolls the song ticker: the
// needles move as often as the ticker steps. So the HUD has a rate of its own:
// 60 a second by default (smoother needles, the ticker twice its original
// speed; the user's choice after playing at 30 and 60, 2026-10-16); 30 as on
// the console (the ticker at its original speed, the needles in 30 Hz steps);
// 120 (the ticker 4 times as fast); or 0 for every frame, as without this
// pacing.
REXCVAR_DEFINE_BOOL(mcla_ui_timing_fix, true, "MCLA",
                    "Advance the UI's menus, pop-ups and map screens 30 times a second, as on the "
                    "console, instead of once per rendered frame");
REXCVAR_DEFINE_INT32(mcla_ui_hud_rate, 60, "MCLA",
                     "How many times a second the in-game HUD advances (gauges, song ticker, "
                     "LOADING badge): 60 (the default) or 120 for smoother needles with a faster "
                     "ticker, 30 as on the console, 0 for every frame")
    .range(0, 240);

// The menus' rate, like the HUD's. 30 is what the console ran them at and what the menus'
// animations were made for. A higher rate makes the highlight, the page changes and the pop-ups
// arrive sooner, and every animation of the UI screens runs that many times as fast (60: twice,
// 120: four times, 0: every frame, as without mcla_ui_timing_fix).
REXCVAR_DEFINE_INT32(mcla_ui_menu_rate, 30, "MCLA",
                     "How many times a second the menus, the pause menu, pop-ups and map panels (the "
                     "UI manager's screens, not the HUD) advance: 30 (the default, as on the console), "
                     "60 or 120 for snappier menus with animations that run 2 or 4 times as fast, 0 for "
                     "every frame")
    .range(0, 240);

// A menu key event (a press, or a repeat of a held direction) is answered on the update it is
// delivered in, without waiting for the screens' next slot (up to 33 ms at 30 a second, 17 on
// average). The key events come from the UI input object (sub_82223CD8, hooks mcla_ui_key_press and
// mcla_ui_key_repeat below), which the UI manager's update runs before it steps the movies, so the
// movies that step in that update show the widgets' reaction (the highlight moving, the new value).
// Each UI screen takes one extra advance per key event and its schedule restarts from it, so the
// animations keep near the console's speed: a tap adds half a movie frame on average, a held
// direction at most 5 extra advances a second. Not the HUD (its own rate), and not an update that
// follows another input advance of the same screen by less than 100 ms. On by default (v0.3.1):
// measured with a scripted pad, the median wait from a key press to the menu's next advance went
// from 8.5 to 0.2 ms at a 120 FPS cap and from 16.9 to 0.2 ms uncapped (launcher/dev/menu-lag-2026-10-29.md).
REXCVAR_DEFINE_BOOL(mcla_ui_advance_on_input, true, "MCLA",
                    "Advance the UI's menus, pop-ups and map screens on the update a menu key event "
                    "(a press or a repeat) is delivered in, instead of waiting for their next slot "
                    "(mcla_ui_menu_rate): the highlight answers a press at once, not 0 to 33 ms later");
REXCVAR_DEFINE_BOOL(mcla_ui_advance_log, false, "MCLA",
                    "Development: log every advance of a UI screen (its name, the time, and whether a "
                    "key event caused it), for measuring how soon the menus answer a press");

// Interpolated UI (development spike, launcher/dev/ui-interpolation-2026-10-29.md). The movies still
// step at their own rate (30 a second for the screens, mcla_ui_hud_rate for the HUD); on every drawn
// frame each display-list element is drawn between what it looked like one step ago and what it looks
// like now, by the fraction of the step period that has passed since the step: its local matrix
// (position, scale, rotation) and its colour transform (alpha, tint). An element that appears,
// disappears or changes character is shown as the step left it, as is one that jumped farther than
// mcla_ui_interp_snap_px or flipped. The picture is one step behind the game's state, which is the
// price of the smoothness (up to 33 ms at 30 steps a second, less with mcla_ui_interp_window_pct). It
// does nothing while the frame time is longer than 0.6 of a step (30 FPS and below for the screens).
REXCVAR_DEFINE_BOOL(mcla_ui_interpolate, false, "MCLA",
                    "Development spike: draw the UI's Flash movies interpolated between their last two "
                    "steps, so that they move smoothly at any frame rate without running faster "
                    "(needs mcla_ui_timing_fix); off by default, untested");
REXCVAR_DEFINE_INT32(mcla_ui_interp_window_pct, 100, "MCLA",
                     "With mcla_ui_interpolate: the share of a step period over which an element "
                     "glides to its new place: 100 (the whole period, smoothest, up to one step of "
                     "delay), less for a shorter glide that settles sooner")
    .range(10, 100);
REXCVAR_DEFINE_INT32(mcla_ui_interp_snap_px, 400, "MCLA",
                     "With mcla_ui_interpolate: an element that moves farther than this many pixels "
                     "of the movie in one step is not glided but shown at once (a cut, a wrap)")
    .range(1, 100000);
REXCVAR_DEFINE_BOOL(mcla_ui_interp_log, false, "MCLA",
                    "Development: log what the interpolated UI does (every element that is gliding, "
                    "each drawn frame) and a self-check of its matrix and colour arithmetic; also "
                    "works with mcla_ui_interpolate off, then it logs what would have been drawn");

namespace {
// The UI manager and its movie map (atMap name -> movie**: buckets at +60,
// count at +64; node: +0 name, +8 movie**, +12 next; movie: +56 player
// context). From LARecomp's notes, used with its author's permission.
constexpr uint32_t kUiManagerPtr = 0x8286D804;

// The last menu key event (a press or a repeat that the UI input object delivers): a counter and the
// time. Written by the key hooks and read by the movie advance, one after the other in the same
// update of the same thread.
std::atomic<uint32_t> g_ui_input_seq{0};
std::atomic<uint64_t> g_ui_input_us{0};
// A key event older than this when a movie comes to look at it belongs to an update long gone (the
// movie was not stepped meanwhile): not answered.
constexpr uint64_t kUiInputWindowUs = 100'000;
// Least time between two extra advances of one screen, whatever the key events do (with
// mcla_ui_repeat_fix off a held direction repeats every 50 ms at 120 FPS).
constexpr uint64_t kUiInputGapUs = 100'000;

uint32_t ReadGuestU32(uint32_t address) {
  if (address < 0x10000) {
    return 0;
  }
  const uint8_t* p = rex::Runtime::instance()->virtual_membase() + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Finds the UI manager's screen that owns a player context. Returns false for a movie outside its map
// (the HUD's gauge movie); else copies the screen's name (HUDMAPMOVIE, PAUSEMOVIE, POPUPMOVIE, ...).
bool UiScreenName(uint32_t context, char (&name)[32]) {
  name[0] = 0;
  const uint32_t manager = ReadGuestU32(kUiManagerPtr);
  if (!manager) {
    return false;
  }
  const uint32_t buckets = ReadGuestU32(manager + 60);
  const uint32_t count = ReadGuestU32(manager + 64) >> 16;
  for (uint32_t i = 0; buckets && i < count && i < 256; i++) {
    uint32_t node = ReadGuestU32(buckets + 4 * i);
    for (int guard = 0; node && guard < 256; guard++, node = ReadGuestU32(node + 12)) {
      const uint32_t holder = ReadGuestU32(node + 8);
      const uint32_t movie = holder ? ReadGuestU32(holder) : 0;
      if (movie && ReadGuestU32(movie + 56) == context) {
        const uint32_t text = ReadGuestU32(node);
        if (text >= 0x10000) {
          const char* s = reinterpret_cast<const char*>(rex::Runtime::instance()->virtual_membase() + text);
          size_t n = 0;
          for (; n + 1 < sizeof name && s[n]; n++) {
            name[n] = s[n];
          }
          name[n] = 0;
        }
        return true;
      }
    }
  }
  return false;
}

// Whether a player context belongs to one of the UI manager's screens other
// than the HUD map.
bool IsUiScreen(uint32_t context, char (&name)[32]) {
  return UiScreenName(context, name) && std::strcmp(name, "HUDMAPMOVIE") != 0;
}

void NoteUiKeyEvent(uint64_t now_us) {
  g_ui_input_us.store(now_us, std::memory_order_relaxed);
  g_ui_input_seq.fetch_add(1, std::memory_order_release);
}

// --- Interpolated UI: what the step hook tells the draw hook (mcla_ui_interpolate) ---
// The swfCONTEXT the player is advancing or drawing: both write it ([0x828FF970], sub_825EDC08 and
// sub_825EE970) and clear it after.
constexpr uint32_t kUiCurrentContext = 0x828FF970;

struct UiStepState {
  std::atomic<uint32_t> context{0};
  std::atomic<uint32_t> serial{0};     // steps of this context noted so far
  std::atomic<uint32_t> period_us{0};  // the nominal time between its steps
  std::atomic<uint64_t> step_us{0};    // when the last step belongs on its schedule
  std::atomic<uint32_t> frame_us{0};   // the time between updates (smoothed), as the hook sees them
  std::atomic<uint64_t> last_call_us{0};
  char name[32]{};
};
std::array<UiStepState, 32> g_ui_step;  // same index as the step hook's schedules

// Every call of the step hook (once per update and movie): keeps the context and the update time.
void TouchUiStep(size_t slot, uint32_t context, uint64_t now_us, const char* name) {
  UiStepState& s = g_ui_step[slot % g_ui_step.size()];
  if (s.context.load(std::memory_order_relaxed) != context) {
    s.context.store(0, std::memory_order_relaxed);
    std::snprintf(s.name, sizeof s.name, "%s", name);
    s.serial.store(0, std::memory_order_relaxed);
    s.frame_us.store(0, std::memory_order_relaxed);
    s.last_call_us.store(0, std::memory_order_relaxed);
    s.context.store(context, std::memory_order_release);
  }
  const uint64_t last = s.last_call_us.load(std::memory_order_relaxed);
  if (last && now_us > last && now_us - last < 250'000) {
    const uint32_t d = uint32_t(now_us - last);
    const uint32_t old = s.frame_us.load(std::memory_order_relaxed);
    s.frame_us.store(old ? (old * 7 + d) / 8 : d, std::memory_order_relaxed);
  }
  s.last_call_us.store(now_us, std::memory_order_relaxed);
}

// A step of the context is certain (one-frame mode steps exactly once per call that is let through).
void NoteUiStep(size_t slot, uint64_t step_us, uint32_t period_us) {
  UiStepState& s = g_ui_step[slot % g_ui_step.size()];
  s.period_us.store(period_us, std::memory_order_relaxed);
  s.step_us.store(step_us, std::memory_order_relaxed);
  s.serial.fetch_add(1, std::memory_order_release);
}
}  // namespace

bool mcla_ui_movie_advance(PPCRegister& r25, PPCRegister& r31) {
  if (OriginalTiming() || !REXCVAR_GET(mcla_ui_timing_fix) || r25.u32 == 0 ||
      !g_engine_timer_started.load(std::memory_order_relaxed)) {
    return false;
  }
  struct Schedule {
    uint32_t context;
    uint64_t next_us;
    uint64_t checked_us;  // when IsUiScreen was last asked: movies come and go
    uint64_t input_us;    // the last extra advance because of a key event
    uint32_t input_seq;   // the last key event this movie has looked at
    bool screen;
    char name[32];
  };
  static std::mutex mutex;
  static std::array<Schedule, 32> schedules{};
  static size_t replace = 0;
  const uint32_t context = r31.u32;
  const uint64_t now = NowMicros();
  std::lock_guard lock(mutex);
  Schedule* schedule = nullptr;
  for (Schedule& s : schedules) {
    if (s.context == context) {
      schedule = &s;
      break;
    }
  }
  if (!schedule) {
    schedule = &schedules[replace++ % schedules.size()];
    *schedule = {context, 0, 0, 0, g_ui_input_seq.load(std::memory_order_acquire), false, {}};
  }
  if (now - schedule->checked_us > 1'000'000) {
    schedule->checked_us = now;
    schedule->screen = IsUiScreen(context, schedule->name);
  }
  const size_t slot = size_t(schedule - schedules.data());
  TouchUiStep(slot, context, now, schedule->name[0] ? schedule->name : "HUDGAUGE");
  const bool log_adv = schedule->screen && REXCVAR_GET(mcla_ui_advance_log);
  uint64_t period_us;
  bool by_input = false;
  if (schedule->screen) {
    const int32_t menu_rate = REXCVAR_GET(mcla_ui_menu_rate);
    if (menu_rate <= 0) {
      if (log_adv) {
        REXLOG_INFO("ui adv: {} ctx={:08X} t={}.{:03} ms in=0", schedule->name, context, now / 1000, now % 1000);
      }
      return false;
    }
    period_us = 1'000'000ull / uint64_t(menu_rate);
    const uint32_t seq = g_ui_input_seq.load(std::memory_order_acquire);
    if (seq != schedule->input_seq) {
      schedule->input_seq = seq;
      // A key event was delivered since this movie was last looked at, in the update that is running
      // now (the input object runs before the movies). Answer it with an advance, unless the movie
      // is due anyway, an earlier answer is too recent, or the event is old.
      by_input = REXCVAR_GET(mcla_ui_advance_on_input) &&
                 now - g_ui_input_us.load(std::memory_order_relaxed) <= kUiInputWindowUs &&
                 now - schedule->input_us >= kUiInputGapUs;
    }
  } else {
    const int32_t hud_rate = REXCVAR_GET(mcla_ui_hud_rate);
    if (hud_rate <= 0) {
      return false;
    }
    period_us = 1'000'000ull / uint64_t(hud_rate);
  }
  const bool not_due = schedule->next_us && now + period_us / 4 < schedule->next_us;
  if (not_due && !by_input) {
    return true;
  }
  uint64_t step_us = now;  // when this step belongs on the schedule (the interpolation's time base)
  if (not_due) {
    // The extra advance: the schedule restarts from it.
    schedule->next_us = now + period_us;
    schedule->input_us = now;
  } else {
    by_input = false;
    const bool behind = !schedule->next_us || now > schedule->next_us + period_us;
    step_us = behind ? now : schedule->next_us;
    schedule->next_us = behind ? now + period_us : schedule->next_us + period_us;
  }
  if (log_adv) {
    REXLOG_INFO("ui adv: {} ctx={:08X} t={}.{:03} ms in={}", schedule->name, context, now / 1000, now % 1000,
                by_input ? 1 : 0);
  }
  // The step is certain from here (one-frame mode steps exactly once): tell the interpolation.
  NoteUiStep(slot, step_us, uint32_t(period_us));
  return false;
}

// Interpolated UI, the draw side (mcla_ui_interpolate). The player's draw walk (sub_825FB348, once per
// display-tree node and drawn frame) computes a node's WORLD matrix and colour transform from its LOCAL
// ones (set by the frame step) and its parent's world ones, then draws the node or walks its children.
// The hook sits right after the colour transform is combined (0x825FB484); for a node whose local values
// changed in the last step it redoes both with the values blended between the old and the new, so the
// node's draw and all its children (which read this node's world values) follow. The local values are not
// touched, so the step (the game's scripts, the next step's deltas) sees what it always saw.
namespace {
uint8_t* GuestBase() {
  return rex::Runtime::instance()->virtual_membase();
}
uint8_t LoadU8(uint32_t a) {
  return a < 0x10000 ? 0 : GuestBase()[a];
}
uint16_t LoadU16(uint32_t a) {
  return a < 0x10000 ? 0 : uint16_t((uint16_t(GuestBase()[a]) << 8) | GuestBase()[a + 1]);
}
float LoadF32(uint32_t a) {
  const uint32_t u = ReadGuestU32(a);
  float f;
  std::memcpy(&f, &u, sizeof f);
  return f;
}
void StoreF32(uint32_t a, float f) {
  if (a < 0x10000) {
    return;
  }
  uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  uint8_t* p = GuestBase() + a;
  p[0] = uint8_t(u >> 24);
  p[1] = uint8_t(u >> 16);
  p[2] = uint8_t(u >> 8);
  p[3] = uint8_t(u);
}
void StoreU8(uint32_t a, uint8_t v) {
  if (a >= 0x10000) {
    GuestBase()[a] = v;
  }
}

// The player's arithmetic (sub_825FB348 at 0x825FB38C-0x825FB454): world = local x parent world, rows
// (a b) (c d) (tx ty); every product rounded to single as the recompiled code does.
void ComposeWorld(const float l[6], const float p[6], float w[6]) {
  auto f = [](double v) { return float(v); };
  w[0] = f(double(l[0]) * p[0] + f(double(l[1]) * p[2]));
  w[1] = f(double(l[0]) * p[1] + f(double(l[1]) * p[3]));
  w[2] = f(double(p[0]) * l[2] + f(double(l[3]) * p[2]));
  w[3] = f(double(p[3]) * l[3] + f(double(p[1]) * l[2]));
  w[4] = f(f(double(p[0]) * l[4] + f(double(l[5]) * p[2])) + p[4]);
  w[5] = f(f(double(p[3]) * l[5] + f(double(p[1]) * l[4])) + p[5]);
}

// sub_825F8B20: world colour transform = local x parent world x tint. Bytes 0-3 multiply (signed, 64 = 1),
// bytes 4-7 add; tint is the parent's four bytes at +68.
void CombineCxform(const int8_t local[8], const uint8_t parent[8], const uint8_t tint[4], uint8_t out[8]) {
  for (int i = 0; i < 4; i++) {
    const int pm = int8_t(parent[i]);
    out[4 + i] = uint8_t(((int(local[4 + i]) * pm) >> 6) + int(parent[4 + i]));
    const int t = int8_t(uint8_t((int(local[i]) * pm) >> 6));
    out[i] = uint8_t((t * int(tint[i])) / 255);
  }
}

struct UiInst {
  uint32_t ctx = 0, def = 0, parent = 0;
  uint16_t depth = 0, charid = 0;
  uint32_t serial = 0;  // the step serial at the last draw of it
  uint64_t t0_us = 0;   // when the glide in progress started (the step it was seen at)
  uint64_t seen_us = 0;
  float from_m[6]{}, to_m[6]{}, shown_m[6]{};
  int8_t from_c[8]{}, to_c[8]{}, shown_c[8]{};
};
std::mutex g_inst_mutex;
std::unordered_map<uint32_t, UiInst> g_inst;
uint64_t g_inst_sweep_us = 0;
// The self-check of the arithmetic (mcla_ui_interp_log): draws compared, mismatches, and the worst error.
uint64_t g_check_n = 0, g_check_matrix_bad = 0, g_check_cx_bad = 0, g_check_glide = 0, g_check_applied = 0;
double g_check_max_err = 0;
uint64_t g_check_log_us = 0;
}  // namespace

void mcla_ui_interp_draw(PPCRegister& r30, PPCRegister& r31) {
  const bool apply_opt = REXCVAR_GET(mcla_ui_interpolate);
  const bool log = REXCVAR_GET(mcla_ui_interp_log);
  if ((!apply_opt && !log) || OriginalTiming() || !REXCVAR_GET(mcla_ui_timing_fix)) {
    return;
  }
  const uint32_t inst = r30.u32;
  const uint32_t parent_s = r31.u32;
  const uint32_t ctx = ReadGuestU32(kUiCurrentContext);
  if (inst < 0x10000 || parent_s < 0x10000 || !ctx) {
    return;
  }
  const UiStepState* step = nullptr;
  for (const UiStepState& s : g_ui_step) {
    if (s.context.load(std::memory_order_acquire) == ctx) {
      step = &s;
      break;
    }
  }
  if (!step) {
    return;  // a movie the step hook does not pace (the 3D icons, the logos): drawn as it is
  }
  const uint32_t serial = step->serial.load(std::memory_order_acquire);
  const uint32_t period_us = step->period_us.load(std::memory_order_relaxed);
  const uint64_t step_us = step->step_us.load(std::memory_order_relaxed);
  const uint32_t frame_us = step->frame_us.load(std::memory_order_relaxed);
  if (!serial || !period_us) {
    return;
  }
  // Worth doing only when several frames are drawn per step; else it would only add delay.
  const bool fast_enough = frame_us && uint64_t(frame_us) * 10 <= uint64_t(period_us) * 6;
  const bool apply = apply_opt && fast_enough;
  const uint64_t now = NowMicros();
  const uint64_t window_us =
      std::max<uint64_t>(1, uint64_t(period_us) * uint64_t(std::clamp(REXCVAR_GET(mcla_ui_interp_window_pct), 10, 100)) / 100);

  float live_m[6];
  int8_t live_c[8];
  for (int k = 0; k < 6; k++) {
    live_m[k] = LoadF32(inst + 8 + 4 * k);
  }
  for (int k = 0; k < 8; k++) {
    live_c[k] = int8_t(LoadU8(inst + 56 + k));
  }
  float parent_w[6];
  uint8_t parent_c[8], tint[4];
  for (int k = 0; k < 6; k++) {
    parent_w[k] = LoadF32(parent_s + 24 + 4 * k);
  }
  for (int k = 0; k < 8; k++) {
    parent_c[k] = LoadU8(parent_s + 56 + k);
  }
  for (int k = 0; k < 4; k++) {
    tint[k] = LoadU8(parent_s + 68 + k);
  }

  std::lock_guard lock(g_inst_mutex);
  if (now - g_inst_sweep_us > 2'000'000) {
    g_inst_sweep_us = now;
    for (auto it = g_inst.begin(); it != g_inst.end();) {
      it = now - it->second.seen_us > 5'000'000 ? g_inst.erase(it) : std::next(it);
    }
  }

  if (log) {
    // The self-check: what the player computed for this node against this code's arithmetic on the same
    // inputs (the local values as they are now). Every mismatch would be an error in the formulas above.
    float w[6];
    ComposeWorld(live_m, parent_w, w);
    double worst = 0;
    for (int k = 0; k < 6; k++) {
      worst = std::max(worst, std::fabs(double(w[k]) - double(LoadF32(inst + 32 + 4 * k))));
    }
    uint8_t cx[8];
    CombineCxform(live_c, parent_c, tint, cx);
    bool cx_bad = false;
    for (int k = 0; k < 8; k++) {
      cx_bad |= cx[k] != LoadU8(inst + 64 + k);
    }
    g_check_n++;
    g_check_matrix_bad += worst > 0.01;
    g_check_cx_bad += cx_bad;
    g_check_max_err = std::max(g_check_max_err, worst);
    if (now - g_check_log_us > 3'000'000) {
      g_check_log_us = now;
      REXLOG_INFO("ui interp check: nodes={} matrix_off={} (worst {:.5f}) cxform_off={} gliding={} applied={}", g_check_n,
                  g_check_matrix_bad, g_check_max_err, g_check_cx_bad, g_check_glide, g_check_applied);
    }
  }

  const uint32_t def = ReadGuestU32(inst + 4);
  const uint16_t depth = LoadU16(inst + 166);
  const uint16_t charid = LoadU16(inst + 168);
  const uint32_t parent = ReadGuestU32(inst + 184);
  UiInst& e = g_inst[inst];
  const bool fresh = e.ctx != ctx || e.def != def || e.depth != depth || e.charid != charid || e.parent != parent;
  if (fresh) {
    // New, or another character at this place: shown as it is.
    e = {};
    e.ctx = ctx;
    e.def = def;
    e.depth = depth;
    e.charid = charid;
    e.parent = parent;
    e.serial = serial;
    e.t0_us = step_us;
    std::memcpy(e.from_m, live_m, sizeof live_m);
    std::memcpy(e.to_m, live_m, sizeof live_m);
    std::memcpy(e.shown_m, live_m, sizeof live_m);
    std::memcpy(e.from_c, live_c, sizeof live_c);
    std::memcpy(e.to_c, live_c, sizeof live_c);
    std::memcpy(e.shown_c, live_c, sizeof live_c);
  } else if (std::memcmp(live_m, e.to_m, sizeof live_m) != 0 || std::memcmp(live_c, e.to_c, sizeof live_c) != 0) {
    // A step changed this node (or something else did): glide from what was shown to the new values,
    // unless steps went by without a draw of it (then nothing to glide from).
    const bool gap = serial - e.serial > 1;
    std::memcpy(e.from_m, gap ? live_m : e.shown_m, sizeof live_m);
    std::memcpy(e.from_c, gap ? live_c : e.shown_c, sizeof live_c);
    std::memcpy(e.to_m, live_m, sizeof live_m);
    std::memcpy(e.to_c, live_c, sizeof live_c);
    e.t0_us = step_us;
    // A cut, a wrap or a flip is not a movement: shown at once.
    const double snap = double(REXCVAR_GET(mcla_ui_interp_snap_px));
    double scale_jump = 0;
    for (int k = 0; k < 4; k++) {
      scale_jump = std::max(scale_jump, std::fabs(double(e.to_m[k]) - double(e.from_m[k])));
    }
    if (std::fabs(double(e.to_m[4]) - e.from_m[4]) > snap || std::fabs(double(e.to_m[5]) - e.from_m[5]) > snap ||
        scale_jump > 0.75) {
      std::memcpy(e.from_m, live_m, sizeof live_m);
    }
  }
  e.serial = serial;
  e.seen_us = now;

  double alpha = 1.0;
  if (now < e.t0_us) {
    alpha = 0.0;
  } else if (now - e.t0_us < window_us) {
    alpha = double(now - e.t0_us) / double(window_us);
  }
  bool m_diff = false, c_diff = false;
  for (int k = 0; k < 6; k++) {
    e.shown_m[k] = alpha >= 1.0 ? e.to_m[k] : float(double(e.from_m[k]) + (double(e.to_m[k]) - e.from_m[k]) * alpha);
    m_diff |= e.shown_m[k] != live_m[k];
  }
  for (int k = 0; k < 8; k++) {
    e.shown_c[k] = alpha >= 1.0 ? e.to_c[k]
                                : int8_t(std::lround(double(e.from_c[k]) + (double(e.to_c[k]) - e.from_c[k]) * alpha));
    c_diff |= e.shown_c[k] != live_c[k];
  }
  if (!m_diff && !c_diff) {
    return;
  }
  if (apply) {
    if (m_diff) {
      float w[6];
      ComposeWorld(e.shown_m, parent_w, w);
      for (int k = 0; k < 6; k++) {
        StoreF32(inst + 32 + 4 * k, w[k]);
      }
    }
    if (c_diff) {
      uint8_t cx[8];
      CombineCxform(e.shown_c, parent_c, tint, cx);
      for (int k = 0; k < 8; k++) {
        StoreU8(inst + 64 + k, cx[k]);
      }
    }
    g_check_applied++;
  }
  g_check_glide++;
  if (log) {
    const uint64_t t = now;
    REXLOG_INFO("ui interp: {} inst={:08X} d={} ser={} t={}.{:03} ms alpha={:.3f} live=({:.2f},{:.2f}) shown=({:.2f},{:.2f}) "
                "world=({:.2f},{:.2f}) applied={}",
                step->name, inst, depth, serial, t / 1000, t % 1000, alpha, live_m[4], live_m[5], e.shown_m[4], e.shown_m[5],
                LoadF32(inst + 48), LoadF32(inst + 52), apply ? 1 : 0);
  }
}

// Held menu buttons repeat by counting updates, not time.
//
// The UI input object (sub_82223CD8, the update of the object the UI manager keeps at +52, once per
// UI manager update) turns the 24 inputs of the menus (d-pad, left stick, A, B, bumpers, ...) into key
// events: a key down on the update a button is first seen pressed (0x822240C0 is that branch), and
// then, for as long as it stays down, a repeat after 7 updates and every [object+803] = 6 updates
// after that (0x822241BC: a byte counter per input at the address in r29, a byte threshold per input
// at r28; the count reaching the threshold sends the key down again). On the console's 30 updates a
// second that is 233 ms, then every 200 ms. At 120 FPS it is 58 ms and then 50 ms, which a press of
// 60 to 150 ms always meets: one tap of "down" moved two or three rows, a tap on a two-value row
// (a checkbox, ON/OFF) flipped it twice and so changed nothing, and a held direction ran at 20 rows
// a second, ahead of the menu's movies, which since mcla_ui_timing_fix move at 30 a second
// (the highlight lags a few rows behind what is selected and a release lands too far).
//
// mcla_ui_repeat_fix waits for the time as well: when the game's count says a repeat is due
// (count + 1 == threshold, the compare at 0x822241D0), the repeat goes ahead only if
// mcla_ui_repeat_delay_ms (after the key down) or mcla_ui_repeat_interval_ms (after the last
// repeat) have passed, less a quarter of a 30 Hz update; otherwise the count is put back by one so
// that the next update asks again, and the compare is made to fail. At 30 FPS and below nothing
// changes (the count is what is slower). The defaults are the console's: 7/30 s and 6/30 s.
// On by default: measured 2026-10-29 (launcher/dev/menu-input-2026-10-29.md), every tap one
// key event at 30, 60, 120 FPS and uncapped, at 3x and with frame generation.
REXCVAR_DEFINE_BOOL(mcla_ui_repeat_fix, true, "MCLA",
                    "Repeat a held menu button or direction after mcla_ui_repeat_delay_ms and then "
                    "every mcla_ui_repeat_interval_ms, as at the console's 30 updates a second, not "
                    "after 7 and every 6 updates (58 and 50 ms at 120 FPS, which made a tap step a "
                    "menu two or three times)");
REXCVAR_DEFINE_INT32(mcla_ui_repeat_delay_ms, 233, "MCLA",
                     "With mcla_ui_repeat_fix: how long a menu button is held before it repeats "
                     "(the game's 7 updates at 30 a second)")
    .range(0, 2000);
REXCVAR_DEFINE_INT32(mcla_ui_repeat_interval_ms, 200, "MCLA",
                     "With mcla_ui_repeat_fix: the time between repeats of a held menu button (the "
                     "game's 6 updates at 30 a second)")
    .range(0, 2000);
REXCVAR_DEFINE_BOOL(mcla_ui_input_log, false, "MCLA",
                    "Development: log every menu key down and repeat (which of the 24 inputs, its "
                    "key code, the time)");

namespace {
constexpr size_t kUiInputCount = 24;

struct UiKeyTime {
  uint64_t last_us = 0;  // the key down, or the last repeat
  uint64_t need_us = 0;  // how long after it the next repeat is due
};
UiKeyTime g_ui_key[kUiInputCount];

void WriteGuestU8(uint32_t address, uint8_t value) {
  if (address < 0x10000) {
    return;
  }
  *(rex::Runtime::instance()->virtual_membase() + address) = value;
}

void LogUiKey(const char* kind, uint32_t input, int32_t key, uint64_t now_us) {
  REXLOG_INFO("ui key: {} input={} key={} t={}.{:03} ms fix={}", kind, input, key, now_us / 1000,
              now_us % 1000, REXCVAR_GET(mcla_ui_repeat_fix));
}
}  // namespace

// 0x822240C0: the branch where a button is first seen pressed (r31 = the input, r4 = its key code or -1).
void mcla_ui_key_press(PPCRegister& r31, PPCRegister& r4) {
  const uint32_t input = r31.u32;
  if (input >= kUiInputCount) {
    return;
  }
  const uint64_t now = NowMicros();
  g_ui_key[input] = {now, uint64_t(std::max(0, REXCVAR_GET(mcla_ui_repeat_delay_ms))) * 1000};
  NoteUiKeyEvent(now);
  if (REXCVAR_GET(mcla_ui_input_log)) {
    LogUiKey("down", input, r4.s32, now);
  }
}

// 0x822241D0: the compare of the count with the threshold (r8 = the count, already stored; r10 =
// the threshold; r29 = the counts' array; r31 = the input; r4 = its key code or -1). Equal: the
// game sends a repeat.
void mcla_ui_key_repeat(PPCRegister& r10, PPCRegister& r8, PPCRegister& r29, PPCRegister& r31,
                        PPCRegister& r4) {
  if (r8.u32 != r10.u32) {
    return;  // no repeat on this update, the usual case
  }
  const uint32_t input = r31.u32;
  if (input >= kUiInputCount) {
    return;
  }
  const uint64_t now = NowMicros();
  UiKeyTime& key = g_ui_key[input];
  if (!OriginalTiming() && REXCVAR_GET(mcla_ui_repeat_fix)) {
    if (!key.last_us) {
      // Down before this was watching (a button held when the menu came up): from now.
      key = {now, uint64_t(std::max(0, REXCVAR_GET(mcla_ui_repeat_delay_ms))) * 1000};
    }
    constexpr int64_t kSlackUs = 1'000'000 / 30 / 4;
    const int64_t late = int64_t(now) - int64_t(key.last_us + key.need_us);
    if (late < -kSlackUs) {
      // Too early: the count one short, so that the next update asks again, and no repeat now.
      WriteGuestU8(r29.u32 + input, uint8_t(r8.u32 - 1));
      r8.u64 = (r10.u32 + 1) & 0xFF;
      return;
    }
    // On time: the next one is due from when this one was due, unless it came a lot later.
    key.last_us = late < int64_t(key.need_us / 2) ? key.last_us + key.need_us : now;
    key.need_us = uint64_t(std::max(0, REXCVAR_GET(mcla_ui_repeat_interval_ms))) * 1000;
  }
  NoteUiKeyEvent(now);
  if (REXCVAR_GET(mcla_ui_input_log)) {
    LogUiKey("repeat", input, r4.s32, now);
  }
}

static bool SkipIntro() {
  return REXCVAR_GET(mcla_skip_intro) || REXCVAR_GET(mcla_intro) == "skip";
}

bool mcla_hook_skip_intro() {
  return SkipIntro();
}

void mcla_skip_intro_render_pass_mask(PPCRegister& r4) {
  if (SkipIntro()) {
    r4.u32 = 0xFEFFFFFFu;
  }
}

void MclaApp::ConfigureFrameTiming() {
  if (OriginalTiming()) {
    REXLOG_INFO("frame timing: original fixed 30 Hz step, vsync left as configured");
    return;
  }
  // Pacing is done by the limiter. With vsync on, a frame that misses a
  // display interval would wait for the next one.
  if (rex::cvar::GetFlagSource("vsync") == rex::cvar::Source::kDefault) {
    rex::cvar::SetFlagByName("vsync", "false");
  }
#ifdef _WIN32
  // Default Windows timer granularity is 15.6 ms; sleeps need 1 ms.
  // Windows 11 stops honouring that request while none of the process's
  // windows can be seen (minimized, covered, or the display asleep), and the
  // limiter's sleeps then overshoot by up to 15.6 ms; opt out of that.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;
  if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling,
                            sizeof(throttling))) {
    REXLOG_INFO("frame timing: 1 ms timer resolution kept while the window is hidden");
  } else {
    REXLOG_WARN("frame timing: could not keep the timer resolution while hidden (error {})",
                GetLastError());
  }
  timeBeginPeriod(1);
#endif
  REXLOG_INFO(
      "frame timing: real frame delta, target {} FPS (mcla_fps; with frame generation on "
      "mcla_fps_frame_generation = {}), vsync {}",
      REXCVAR_GET(mcla_fps), REXCVAR_GET(mcla_fps_frame_generation),
      rex::cvar::GetFlagByName("vsync"));
}

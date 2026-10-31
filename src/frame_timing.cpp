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
#include "pc_triggers.h"
#include "vibration.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// src/pad_probe.cpp: the title screen's frame-rate cap (mcla_title_fps), applied to the player's cap.
int32_t MclaTitleFpsCap(int32_t cap);

namespace {

// The engine timer object lives at a fixed guest address (sub_821BDA90).
constexpr uint32_t kGuestFrameDelta = 0x827D7508;  // seconds, time-scaled
constexpr uint32_t kGuestFrameRate = 0x827D750C;   // 1 / delta
constexpr uint32_t kGuestTimer = 0x827D7500;       // the timer object's base: +16 time since the mode loaded, +20 total, +32 fixed step

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

// Once per engine update (mcla_ui_pace_log; defined with the UI movie hooks below).
void UiPaceLogTick();

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
    const int32_t value = MclaTitleFpsCap(CapFromSettings());
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
#if !REX_PLATFORM_PS5
  MclaPcTriggersFrame();  // the DualSense's adaptive triggers (src/pc_triggers.cpp)
#endif
  MclaVibrationFrame();  // the controllers follow mcla_vibration (src/vibration.cpp)
  MclaDevPadFrame(r8.u64);
  UiPaceLogTick();
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
// needles move as often as the ticker steps. So the HUD has a rate of its own,
// which counts while mcla_ui_interpolate (below) is off: 60 a second by default
// (smoother needles, the ticker twice its original speed; the user's choice
// after playing at 30 and 60, 2026-10-16); 30 as on the console (the ticker at
// its original speed, the needles in 30 Hz steps); 120 (the ticker 4 times as
// fast); or 0 for every frame, as without this pacing. With the interpolation
// on (the default since v0.3.1) the HUD steps 30 times a second like every
// other movie and the picture between the steps is blended.
REXCVAR_DEFINE_BOOL(mcla_ui_timing_fix, true, "MCLA",
                    "Advance the UI's menus, pop-ups and map screens 30 times a second, as on the "
                    "console, instead of once per rendered frame");
REXCVAR_DEFINE_INT32(mcla_ui_hud_rate, 60, "MCLA",
                     "While mcla_ui_interpolate is off: how many times a second the in-game HUD "
                     "advances (gauges, song ticker, LOADING badge): 60 (the default) or 120 for "
                     "smoother needles with a faster ticker, 30 as on the console, 0 for every frame")
    .range(0, 240);

// The menus' rate, like the HUD's, while mcla_ui_interpolate is off. 30 is what the console ran
// them at and what the menus' animations were made for. A higher rate makes the highlight, the
// page changes and the pop-ups arrive sooner, and every animation of the UI screens runs that
// many times as fast (60: twice, 120: four times, 0: every frame, as without mcla_ui_timing_fix).
REXCVAR_DEFINE_INT32(mcla_ui_menu_rate, 30, "MCLA",
                     "While mcla_ui_interpolate is off: how many times a second the menus, the pause "
                     "menu, pop-ups and map panels (the UI manager's screens, not the HUD) advance: 30 "
                     "(the default, as on the console), 60 or 120 for snappier menus with animations that "
                     "run 2 or 4 times as fast, 0 for every frame")
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

// Interpolated UI (launcher/dev/ui-interpolation-2026-10-29.md; on by default since v0.3.1, the
// pause menu's DISPLAY > SMOOTH UI). Every movie steps 30 times a second, the console's rate (the
// two rate options above count only while this is off); on every drawn
// frame each display-list element is drawn between what it looked like one step ago and what it looks
// like now, by the fraction of the step period that has passed since the step: its local matrix
// (position, scale, rotation) and its colour transform (alpha, tint). An element that appears,
// disappears or changes character is shown as the step left it, as is one that jumped farther than
// mcla_ui_interp_snap_px or flipped. The picture is one step behind the game's state, which is the
// price of the smoothness (up to 33 ms at 30 steps a second, less with mcla_ui_interp_window_pct). It
// does nothing while the frame time is longer than 0.6 of a step (30 FPS and below for the screens).
REXCVAR_DEFINE_BOOL(mcla_ui_interpolate, true, "MCLA",
                    "Draw the UI's Flash movies (menus, pop-ups, map panels and the HUD) interpolated "
                    "between their last two steps, so that they move smoothly at any frame rate without "
                    "running faster; every movie steps 30 times a second then, and mcla_ui_menu_rate and "
                    "mcla_ui_hud_rate are not read (needs mcla_ui_timing_fix)");
REXCVAR_DEFINE_INT32(mcla_ui_interp_window_pct, 100, "MCLA",
                     "With mcla_ui_interpolate: the share of a step period over which an element "
                     "glides to its new place: 100 (the whole period, smoothest, up to one step of "
                     "delay), less for a shorter glide that settles sooner")
    .range(10, 100);
REXCVAR_DEFINE_INT32(mcla_ui_interp_snap_px, 160, "MCLA",
                     "With mcla_ui_interpolate: an element that moves farther than this many pixels "
                     "of the movie in one step is not glided but shown at once (a cut, a wrap); one "
                     "that turns round and jumps more than 40 px is shown at once too (a ticker "
                     "wrapping round)")
    .range(1, 100000);
REXCVAR_DEFINE_BOOL(mcla_ui_interp_log, false, "MCLA",
                    "Development: log what the interpolated UI does (every element that is gliding, "
                    "each drawn frame), the elements that snap or change character, and a self-check of "
                    "its matrix and colour arithmetic; also works with mcla_ui_interpolate off, then it "
                    "logs what would have been drawn");
REXCVAR_DEFINE_BOOL(mcla_ui_interp_stats, false, "MCLA",
                    "Development: once a second, log how long the interpolation's hook took (calls, "
                    "total and worst time), for measuring its cost; cheap enough to leave on while "
                    "measuring frame times (the logging options above are not)");

// The pause menu is not only 2D: it moves a camera and 3D panels (below). Those are not display-list
// elements, so the draw hook above never sees them. They come from the movie's script, which sets a few
// numbers in 30 Hz steps; the game reads them again on every update (the camera) and every drawn frame (the
// panels). The reads are blended here as the elements are above: one step behind, the same fraction of the
// step.
REXCVAR_DEFINE_BOOL(mcla_ui_interp_camera, true, "MCLA",
                    "With mcla_ui_interpolate: also draw the pause menu's 3D between the movie's last two "
                    "steps: the camera that swings in when the menu opens, between its pages and out "
                    "again, and the place, size and turn of the 3D panels. Off leaves them in 30 Hz steps");

// Race finish check (launcher/dev/race-finish-2026-10-29.md). LARecomp (mzzvxm/LARecomp, commit 7b9726e) reports
// that with the 30 Hz pacing of the UI movies the race finish screens, the podium and the results run in slow
// motion, and exempts movies with HUD, RACE, POST, RESULT, PODIUM, CINE or END in their names from it. Whether
// that happens in this build, and with which movie, is for the measurement (mcla_ui_pace_log). Until it is known
// these two options change nothing by default.
// Race start check (launcher/dev/race-start-2026-10-29.md). The owner sees the race start while the "2" (cap 120, HUD
// stepped 60 a second) or the "3" (Smooth UI, every movie 30 a second) of the countdown is still on screen. The rule
// below is the tool for it: a rate for the movies it names, kept by a schedule that catches up (several movie frames
// in one update when it is behind) and, if asked, counted in the game's own seconds.
REXCVAR_DEFINE_STRING(mcla_ui_movie_rates, "", "MCLA",
                      "Development: how often chosen UI movies step, instead of the usual rule (30 with "
                      "mcla_ui_interpolate, else mcla_ui_menu_rate and mcla_ui_hud_rate). A comma list of "
                      "NAME=RATE: NAME is a movie's name (POPUPMOVIE; a trailing * matches a prefix, a lone "
                      "* every movie, HUDGAUGE is the gauge movie of the HUD); RATE is a number of steps a "
                      "second, 0 (every update, as without mcla_ui_timing_fix), 'game' (the inverse of the "
                      "engine timer's own step, 30 on the console) or 'swf' (the movie's own frame rate); "
                      "g:RATE (g:30, g:swf) counts the seconds on the game's clock (the engine timer's total "
                      "time) instead of the wall clock, so the movie keeps to the game through a slowdown. A "
                      "named movie that is behind steps several frames in one update to catch up (up to 8); "
                      "the interpolation glides over the whole change. Empty (the default): the usual rule "
                      "for every movie");
REXCVAR_DEFINE_BOOL(mcla_ui_pace_log, false, "MCLA",
                    "Development: once a second (mcla_ui_pace_log_ms), log how many engine updates there "
                    "were, for each UI movie how many times the engine asked to advance it and how many "
                    "frames it stepped, and the engine timer's clocks (for telling what runs at what speed in "
                    "the race start and finish sequences)");
REXCVAR_DEFINE_INT32(mcla_ui_pace_log_ms, 1000, "MCLA",
                     "Development: the length of the window of each mcla_ui_pace_log line, in milliseconds "
                     "(1000; 50 to 100 for the race start, which the dev pad sets with `set`)")
    .range(20, 10000);

// The race-start countdown (the ring in the middle of the screen: 3, 2, 1, GO) is timed by the HUD movie's own script, not by
// the game's clock. Every update the HUD sets the ActionScript variable `countdownUpdate` of the HUD gauge movie (hud/hud.swf,
// logged as HUDGAUGE) to that update's elapsed time (sub_82212298, 0x822124E0), and the script adds the variable to its
// countdown clock on every movie STEP. On the console (30 updates, 30 steps a second) each step added 1/30 s; with the movie
// paced below the update rate a step saw only the last update's value, so the clock ran at steps/updates of the game's speed:
// every number lasted 2 s with the HUD at 60 steps a second at a 120 FPS cap and 4 s with Smooth UI's 30 (measured,
// launcher/dev/race-countdown-2026-10-29.md). The fix gives each step the time of all the updates since the movie's last step.
REXCVAR_DEFINE_BOOL(mcla_ui_countdown_fix, true, "MCLA",
                    "Keep the race-start countdown (3, 2, 1, GO) in time with the race at any frame rate: the HUD "
                    "movie's countdown clock gets the game time of all the updates since its last step, not of the "
                    "last update only (without it the countdown runs at the movie's steps per update, 1/4 speed "
                    "with Smooth UI at a 120 FPS cap). Needs mcla_ui_timing_fix to have any effect");

namespace {
// The UI manager and its movie map (atMap name -> movie**: buckets at +60,
// count at +64; node: +0 name, +8 movie**, +12 next; movie: +56 player
// context). From LARecomp's notes.
constexpr uint32_t kUiManagerPtr = 0x8286D804;

// Steps a second of every UI movie with mcla_ui_interpolate on (all the SWF files seen declare 30 frames a second).
constexpr int32_t kUiInterpSteps = 30;

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

// The last time the pause menu's movie came up for an advance: the movie is advanced every update while the
// pause menu is up and not at all in free roam or a race (measured in the menu-lag run of 2026-10-29: 2.6 s of
// free roam, no advance of any screen; then PAUSEMOVIE every 33 ms from the Start press on).
std::atomic<uint64_t> g_pause_movie_us{0};

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
  std::atomic<uint32_t> calls{0};      // updates that reached the hook (one per drawn frame, as the draw sees it)
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
  s.calls.fetch_add(1, std::memory_order_relaxed);
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

// --- mcla_ui_movie_rates: a rate for chosen movies ---
constexpr int32_t kRateNone = -1;  // no entry for the movie: the usual rule
constexpr int32_t kRateGame = -2;  // 'game': the inverse of the engine timer's own step
constexpr int32_t kRateSwf = -3;   // 'swf': the movie's own frame rate
// Most movie frames one update may step for a movie with a rate of its own (when it is behind).
constexpr int kMaxCatchUpSteps = 8;

struct UiRateRule {
  int32_t rate = kRateNone;  // steps a second, 0, kRateGame or kRateSwf; kRateNone: no entry
  bool game_clock = false;   // g:RATE: the seconds are the game's (engine timer total time), not the wall clock's
};

std::string TrimmedCopy(const std::string& text) {
  size_t begin = 0, end = text.size();
  while (begin < end && text[begin] == ' ') {
    begin++;
  }
  while (end > begin && text[end - 1] == ' ') {
    end--;
  }
  return text.substr(begin, end - begin);
}

// The rule the list gives the movie called name (the first item that matches), kRateNone if it gives none.
UiRateRule UiMovieRateFor(const std::string& list, const char* name) {
  size_t pos = 0;
  while (pos <= list.size()) {
    size_t end = list.find(',', pos);
    if (end == std::string::npos) {
      end = list.size();
    }
    const std::string item = list.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = item.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string pattern = TrimmedCopy(item.substr(0, eq));
    std::string value = TrimmedCopy(item.substr(eq + 1));
    bool match = pattern == name;
    if (!match && !pattern.empty() && pattern.back() == '*') {
      match = std::strncmp(name, pattern.c_str(), pattern.size() - 1) == 0;
    }
    if (!match) {
      continue;
    }
    UiRateRule rule;
    if (value.rfind("g:", 0) == 0) {
      rule.game_clock = true;
      value = TrimmedCopy(value.substr(2));
    }
    if (value == "game") {
      rule.rate = kRateGame;
    } else if (value == "swf") {
      rule.rate = kRateSwf;
    } else {
      char* tail = nullptr;
      const long rate = std::strtol(value.c_str(), &tail, 10);
      if (tail != value.c_str() && *tail == 0 && rate >= 0 && rate <= 240) {
        rule.rate = int32_t(rate);
      }
    }
    return rule;
  }
  return {};
}

// What the engine timer's own step says the game runs at: [timer + 32] is its fixed step in seconds (1/30 on the
// console), kept from before this build replaced it with the real elapsed time. Not a step (0, or something odd): fallback.
int32_t GameStepRate(int32_t fallback) {
  const float step = ReadGuestFloat(kGuestTimer + 32);
  if (step > 1.0f / 240.0f && step < 1.0f / 15.0f) {
    return int32_t(std::lround(1.0 / double(step)));
  }
  return fallback;
}

// The frame rate the movie's SWF file declares (the halfword at +48 of its definition, which the advance divides
// into 1 s to get the length of a movie frame), the fallback if it is not a usable rate.
int32_t SwfFrameRate(uint32_t context, int32_t fallback) {
  const uint32_t definition = ReadGuestU32(context + 28);
  const uint32_t fps = definition ? (ReadGuestU32(definition + 48) >> 16) : 0;
  return (fps >= 1 && fps <= 240) ? int32_t(fps) : fallback;
}

// The game's clock in microseconds: the engine timer's total time (+20), which advances by the time-scaled elapsed
// time of every update and starts again from 0 when a mode loads.
uint64_t GameClockUs() {
  const double seconds = ReadGuestFloat(kGuestTimer + 20);
  return seconds > 0.0 && seconds < 1.0e6 ? uint64_t(seconds * 1.0e6) : 0;
}

void WriteGuestFloat(uint32_t address, float value) {
  if (address < 0x10000) {
    return;
  }
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  uint8_t* p = rex::Runtime::instance()->virtual_membase() + address;
  p[0] = uint8_t(bits >> 24);
  p[1] = uint8_t(bits >> 16);
  p[2] = uint8_t(bits >> 8);
  p[3] = uint8_t(bits);
}

// --- mcla_ui_countdown_fix: the HUD gauge movie's `countdownUpdate` variable ---
// What the game wrote since the movie appeared (`total`) and the part of it a movie step has taken (`consumed`), in seconds.
// The write hook gives the variable total - consumed (the time since the last step); the step hook takes it.
struct HudDtFeed {
  std::mutex mutex;
  uint32_t context = 0;   // the HUD gauge movie's swfCONTEXT, as the write hook saw it
  double total = 0.0;
  double consumed = 0.0;
  double fed = 0.0;       // the value the variable holds now (what the next step of the movie reads)
  uint32_t since_step = 0;  // writes since a step of this movie was last seen
  // The pace log's window (mcla_ui_pace_log): writes, the game's own values summed, steps, the values the steps read summed.
  uint32_t writes = 0;
  double written_sum = 0.0;
  uint32_t steps = 0;
  double fed_sum = 0.0;
};
HudDtFeed g_hud_dt;
std::atomic<uint32_t> g_hud_dt_context{0};  // g_hud_dt.context, readable without the lock (every movie's advance asks)
// The write hook feeds only a movie that is being stepped: with no step seen for this many writes (a HUD that is hidden, or a
// context that is not the one the advance sees) it leaves the game's value alone rather than feed a clock nobody reads.
constexpr uint32_t kHudDtLiveWrites = 100;

// A call to advance `context` was let through (the movie steps): the HUD gauge movie takes the time it was given.
void HudDtNoteStep(uint32_t context) {
  if (context != g_hud_dt_context.load(std::memory_order_relaxed)) {
    return;
  }
  std::lock_guard lock(g_hud_dt.mutex);
  if (context != g_hud_dt.context) {
    return;
  }
  g_hud_dt.since_step = 0;
  g_hud_dt.consumed = g_hud_dt.total;
  g_hud_dt.steps++;
  g_hud_dt.fed_sum += g_hud_dt.fed;
}

struct HudDtWindow {
  uint32_t writes, steps;
  double written_sum, fed_sum;
};
HudDtWindow HudDtTakeWindow() {
  std::lock_guard lock(g_hud_dt.mutex);
  const HudDtWindow w{g_hud_dt.writes, g_hud_dt.steps, g_hud_dt.written_sum, g_hud_dt.fed_sum};
  g_hud_dt.writes = g_hud_dt.steps = 0;
  g_hud_dt.written_sum = g_hud_dt.fed_sum = 0.0;
  return w;
}

// --- mcla_ui_pace_log: what the engine asks of the UI movies, and what the timer's clocks do ---
struct UiPaceCount {
  uint32_t context = 0;
  uint32_t calls = 0;     // asks to advance in the one-frame mode (the mode the UI manager uses)
  uint32_t steps = 0;     // movie frames stepped (the asks that were let through, and the frames a catch-up added)
  uint32_t dt_calls = 0;  // asks in the elapsed-time mode (never held back by the pacing)
};
std::mutex g_pace_mutex;
std::array<UiPaceCount, 48> g_pace;
uint32_t g_pace_updates = 0;
uint64_t g_pace_window_us = 0;

void CountUiAdvance(uint32_t context, bool one_frame, uint32_t stepped) {
  std::lock_guard lock(g_pace_mutex);
  UiPaceCount* slot = nullptr;
  UiPaceCount* spare = nullptr;
  for (UiPaceCount& c : g_pace) {
    if (c.context == context) {
      slot = &c;
      break;
    }
    if (!c.context && !spare) {
      spare = &c;
    }
  }
  if (!slot) {
    slot = spare ? spare : &g_pace[context % g_pace.size()];
    *slot = {};
    slot->context = context;
  }
  if (one_frame) {
    slot->calls++;
    slot->steps += stepped;
  } else {
    slot->dt_calls++;
  }
}
}  // namespace

// For the DualSense triggers' release (src/pc_triggers.cpp): the pause menu is up while its movie keeps coming
// up for an advance (every 33 ms at the default menu rate), even with the car standing still. Needs
// mcla_ui_timing_fix (this hook returns before the note when it is off).
bool MclaPauseMenuUp() {
  const uint64_t last = g_pause_movie_us.load(std::memory_order_relaxed);
  return last != 0 && NowMicros() - last < 250'000;
}

namespace {
void UiPaceLogTick() {
  if (!REXCVAR_GET(mcla_ui_pace_log)) {
    return;
  }
  const uint64_t now = NowMicros();
  std::lock_guard lock(g_pace_mutex);
  g_pace_updates++;
  if (!g_pace_window_us) {
    g_pace_window_us = now;
    g_pace_updates = 0;
    return;
  }
  if (now - g_pace_window_us < uint64_t(REXCVAR_GET(mcla_ui_pace_log_ms)) * 1000) {
    return;
  }
  std::string movies;
  for (UiPaceCount& c : g_pace) {
    if (!c.context) {
      continue;
    }
    if (!c.calls && !c.dt_calls) {
      c = {};  // not asked about in this window: the movie is gone
      continue;
    }
    char name[32];
    if (!UiScreenName(c.context, name)) {
      std::snprintf(name, sizeof name, "HUDGAUGE");
    }
    const uint32_t definition = ReadGuestU32(c.context + 28);
    const uint32_t swf_fps = definition ? (ReadGuestU32(definition + 48) >> 16) : 0;
    char entry[96];
    std::snprintf(entry, sizeof entry, " %s:%08X:%u:%u/%u/%u", name, c.context, swf_fps, c.calls, c.steps, c.dt_calls);
    movies += entry;
    c.calls = c.steps = c.dt_calls = 0;
  }
  const double span_ms = double(now - g_pace_window_us) / 1000.0;
  const HudDtWindow cd = HudDtTakeWindow();
  REXLOG_INFO(
      "ui pace: span_ms={:.1f} updates={} movies(name:ctx:swf_fps:calls/steps/dtcalls){} | timer dt={:.5f} rate={:.2f} "
      "mode_t={:.3f} total_t={:.3f} total2_t={:.3f} step={:.5f} scale={:.3f} frames={} state={} us={} cdn={} cdwrit={:.4f} "
      "cdsteps={} cdfed={:.4f}",
      span_ms, g_pace_updates, movies.empty() ? " none" : movies, ReadGuestFloat(kGuestTimer + 8),
      ReadGuestFloat(kGuestTimer + 12), ReadGuestFloat(kGuestTimer + 16), ReadGuestFloat(kGuestTimer + 20),
      ReadGuestFloat(kGuestTimer + 24), ReadGuestFloat(kGuestTimer + 32), ReadGuestFloat(kGuestTimer + 84),
      ReadGuestU32(kGuestTimer + 44), ReadGuestU32(0x8288D000), now, cd.writes, cd.written_sum, cd.steps, cd.fed_sum);
  g_pace_window_us = now;
  g_pace_updates = 0;
}
}  // namespace

static bool UiMovieAdvance(PPCRegister& r25, PPCRegister& r31, PPCRegister& f30, PPCRegister& f31, PPCCRRegister& cr6,
                           uint32_t& frames);

// The hook (config/frame_timing.toml): r25 is the one-frame flag, r31 the movie's player context, f30 the elapsed time
// the caller passed and f31 the length of one movie frame (both in seconds), cr6 the compare of r25 with 0 that the
// next instruction branches on. True holds the advance back; for a movie with a rule of its own that is behind, the
// hook flips the call into the elapsed-time mode (r25 and cr6) with an accumulator that makes it step several frames.
bool mcla_ui_movie_advance(PPCRegister& r25, PPCRegister& r31, PPCRegister& f30, PPCRegister& f31,
                           PPCCRRegister& cr6) {
  const bool one_frame = r25.u32 != 0;
  uint32_t frames = 1;  // movie frames this call steps, if it is let through
  const bool hold = UiMovieAdvance(r25, r31, f30, f31, cr6, frames);
  if (REXCVAR_GET(mcla_ui_pace_log)) {
    CountUiAdvance(r31.u32, one_frame, hold ? 0 : frames);
  }
  if (!hold) {
    HudDtNoteStep(r31.u32);  // mcla_ui_countdown_fix: the HUD gauge movie takes the time it was given
  }
  return hold;
}

// The hook (config/frame_timing.toml) at 0x822124DC in sub_82212298, the HUD's update: r3 is the HUD gauge movie's
// swfCONTEXT and f1 the value of its `countdownUpdate` variable, the update's elapsed time (the call that sets the
// variable follows, sub_825EE188). The variable is read once per movie step, so it gets the time since the last step.
void mcla_ui_countdown_dt(PPCRegister& r3, PPCRegister& f1) {
  if (OriginalTiming()) {
    return;
  }
  const double value = f1.f64;
  if (!(value >= 0.0 && value < 1.0)) {
    return;  // not a time: leave it to the game
  }
  const bool fix = REXCVAR_GET(mcla_ui_countdown_fix);
  std::lock_guard lock(g_hud_dt.mutex);
  if (g_hud_dt.context != r3.u32) {
    REXLOG_INFO("ui countdown: the HUD movie is now ctx={:08X} (was {:08X}), first countdownUpdate {:.5f}", r3.u32,
                g_hud_dt.context, value);
    g_hud_dt.context = r3.u32;
    g_hud_dt_context.store(r3.u32, std::memory_order_relaxed);
    g_hud_dt.total = g_hud_dt.consumed = 0.0;
    g_hud_dt.since_step = 0;
  }
  g_hud_dt.total += value;
  g_hud_dt.since_step++;
  double fed = value;
  if (fix && g_hud_dt.since_step <= kHudDtLiveWrites) {
    // The time since the last step: at least this update's own value and at most what one engine frame may advance (a
    // movie that is not stepped at all, a HUD that is hidden, must not give its first step minutes at once).
    fed = std::max(value, std::min(g_hud_dt.total - g_hud_dt.consumed, std::max(value, kMaxFrameSeconds)));
    fed = double(float(fed));
    f1.f64 = fed;
  }
  g_hud_dt.fed = fed;
  g_hud_dt.writes++;
  g_hud_dt.written_sum += value;
}

// The movie has a rule in mcla_ui_movie_rates: its own schedule, in the wall clock or the game's, that steps as many
// frames as are due (up to kMaxCatchUpSteps) and tells the interpolation. `next` is when the next frame is due, in the
// clock the rule counts in (next_game_clock says which). Returns true to hold the call back.
static bool UiRuleAdvance(UiRateRule rule, uint32_t context, size_t slot, uint64_t now, uint64_t& next,
                          bool& next_game_clock, PPCRegister& r25, PPCRegister& f30, PPCRegister& f31,
                          PPCCRRegister& cr6, uint32_t& frames) {
  int32_t rate = rule.rate == kRateGame ? GameStepRate(30) : rule.rate == kRateSwf ? SwfFrameRate(context, 30) : rule.rate;
  if (rate <= 0) {
    return false;  // every update
  }
  const uint64_t period = 1'000'000ull / uint64_t(rate);
  const uint64_t clock = rule.game_clock ? GameClockUs() : now;
  if (!next || next_game_clock != rule.game_clock || (rule.game_clock && next > clock + 2 * period + 1'000'000)) {
    // First call, the clock changed, or the game's clock started again (a mode loaded): due now.
    next = clock;
    next_game_clock = rule.game_clock;
  }
  uint32_t due = 0;
  while (due < uint32_t(kMaxCatchUpSteps) && clock + period / 4 >= next) {
    due++;
    next += period;
  }
  if (due == 0) {
    return true;
  }
  if (due == uint32_t(kMaxCatchUpSteps) && clock + period / 4 >= next) {
    next = clock + period;  // far behind (a long stall): the rest of the backlog is dropped
  }
  frames = due;
  if (due > 1) {
    // Several frames in this call: the elapsed-time mode steps while its accumulator holds a whole frame. The
    // accumulator is [movie + 24]; the mode adds the caller's elapsed time to it before the loop, so it is set to
    // (due + 0.5) frames less that, and the loop steps exactly `due` times, leaving half a frame.
    double elapsed = f30.f64;
    if (!(elapsed >= 0.0 && elapsed < 1.0)) {
      elapsed = 0.0;
    }
    WriteGuestFloat(context + 24, float((double(due) + 0.5) * f31.f64 - elapsed));
    r25.u32 = 0;
    cr6.eq = 1;
    cr6.lt = 0;
    cr6.gt = 0;
  }
  // The step belongs on its schedule when the wall clock keeps it, else now (the interpolation's time base).
  const uint64_t ideal = next - period;
  NoteUiStep(slot, (!rule.game_clock && ideal <= now && now - ideal <= period) ? ideal : now, uint32_t(period));
  return false;
}

static bool UiMovieAdvance(PPCRegister& r25, PPCRegister& r31, PPCRegister& f30, PPCRegister& f31, PPCCRRegister& cr6,
                           uint32_t& frames) {
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
    bool pause;  // the pause menu's movie (PAUSEMOVIE)
    UiRateRule rule;      // mcla_ui_movie_rates for this movie (rate kRateNone: none)
    uint64_t rule_next;   // with a rule: when its next frame is due, in the clock the rule counts in
    bool rule_clock;      // which clock rule_next is in (true: the game's)
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
    *schedule = Schedule{};
    schedule->context = context;
    schedule->input_seq = g_ui_input_seq.load(std::memory_order_acquire);
  }
  if (now - schedule->checked_us > 1'000'000) {
    schedule->checked_us = now;
    schedule->screen = IsUiScreen(context, schedule->name);
    schedule->pause = schedule->screen && std::strcmp(schedule->name, "PAUSEMOVIE") == 0;
    const std::string rates = REXCVAR_GET(mcla_ui_movie_rates);
    schedule->rule = rates.empty() ? UiRateRule{} : UiMovieRateFor(rates, schedule->name[0] ? schedule->name : "HUDGAUGE");
  }
  if (schedule->pause) {
    g_pause_movie_us.store(now, std::memory_order_relaxed);
  }
  const size_t slot = size_t(schedule - schedules.data());
  TouchUiStep(slot, context, now, schedule->name[0] ? schedule->name : "HUDGAUGE");
  const bool log_adv = schedule->screen && REXCVAR_GET(mcla_ui_advance_log);
  if (schedule->rule.rate != kRateNone) {
    // A movie with a rule of its own: its schedule, catching up (no menu key answers: it is a chosen movie).
    return UiRuleAdvance(schedule->rule, context, slot, now, schedule->rule_next, schedule->rule_clock, r25, f30, f31,
                         cr6, frames);
  }
  uint64_t period_us;
  bool by_input = false;
  // With the interpolation on, every movie steps 30 times a second (the SWF files' own rate, the console's)
  // whatever the two rate options say: the glide supplies the frames in between, nothing runs faster.
  const bool smooth = REXCVAR_GET(mcla_ui_interpolate);
  if (schedule->screen) {
    const int32_t menu_rate = smooth ? kUiInterpSteps : REXCVAR_GET(mcla_ui_menu_rate);
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
    const int32_t hud_rate = smooth ? kUiInterpSteps : REXCVAR_GET(mcla_ui_hud_rate);
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

// A step that moves a node farther than this, against the way it moved in the step before and more than 8 times as far,
// is a wrap, not a movement (the HUD's ticker comes in again at the far end: 373 px in one step, measured).
constexpr double kInterpTurnPx = 40.0;

struct UiInst {
  uint32_t ctx = 0, def = 0, parent = 0;
  uint16_t depth = 0, charid = 0;
  uint32_t serial = 0;  // the step serial at the last draw of it
  uint64_t t0_us = 0;   // when the glide in progress started (the step it was seen at)
  uint64_t seen_us = 0;
  uint32_t changed_serial = 0;  // the step that last changed its matrix or colour transform
  double last_dx = 0, last_dy = 0;  // how far that step moved it (tx, ty)
  uint32_t swaps = 0;   // how often another character took this place
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
// What each step changed, per context (mcla_ui_interp_log): how many nodes moved, changed their colour transform,
// were new (fresh) or another character (swapped; both are shown as they are), jumped (snapped), and the largest move.
struct UiCensus {
  uint32_t serial = 0;
  uint32_t moved = 0, cx = 0, fresh = 0, swapped = 0, snapped = 0, gap = 0;
  double max_move = 0;
};
std::array<UiCensus, 32> g_census;
// One time for all the nodes of a frame (and of the passes that draw it more than once): the update it
// belongs to and the time of its first node. Else a mask and what it masks, drawn a moment apart, would
// glide a fraction of a pixel apart.
struct UiFrameTime {
  uint32_t calls = 0;
  uint64_t now_us = 0;
};
std::array<UiFrameTime, 32> g_frame_time;
}  // namespace

namespace {
void UiInterpDraw(PPCRegister& r30, PPCRegister& r31, bool apply_opt, bool log) {
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
  const uint32_t calls = step->calls.load(std::memory_order_relaxed);
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
  if (g_inst.bucket_count() < 4096) {
    g_inst.reserve(4096);  // a menu opening adds a few hundred nodes at once: no rehash in the middle of a frame
  }
  UiFrameTime& frame_time = g_frame_time[size_t(step - g_ui_step.data()) % g_frame_time.size()];
  const uint64_t real_now = NowMicros();
  if (frame_time.calls != calls || real_now - frame_time.now_us > 6000) {
    frame_time = {calls, real_now};
  }
  const uint64_t now = frame_time.now_us;
  UiCensus* census = nullptr;
  if (log) {
    census = &g_census[size_t(step - g_ui_step.data()) % g_census.size()];
    if (census->serial != serial) {
      if (census->serial) {
        size_t tracked = 0;
        for (const auto& kv : g_inst) {
          tracked += kv.second.ctx == ctx;
        }
        REXLOG_INFO("ui interp step: {} ser={} moved={} cx={} fresh={} swapped={} snapped={} gap={} max_move={:.1f}px tracked={}",
                    step->name, census->serial, census->moved, census->cx, census->fresh, census->swapped, census->snapped,
                    census->gap, census->max_move, tracked);
      }
      if (!census->serial) {
        // First time this context is seen drawn: its file's frame rate (the u16 at [[ctx+28]+48]), for the notes.
        REXLOG_INFO("ui interp movie: {} ctx={:08X} swf_fps={} period={} us", step->name, ctx,
                    LoadU16(ReadGuestU32(ctx + 28) + 48), period_us);
      }
      *census = {};
      census->serial = serial;
    }
  }
  if (real_now - g_inst_sweep_us > 2'000'000) {
    g_inst_sweep_us = real_now;
    for (auto it = g_inst.begin(); it != g_inst.end();) {
      it = real_now - it->second.seen_us > 5'000'000 ? g_inst.erase(it) : std::next(it);
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
    if (real_now - g_check_log_us > 3'000'000) {
      g_check_log_us = real_now;
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
    const bool brand_new = e.ctx != ctx;
    const uint32_t swaps = brand_new ? 0 : e.swaps + 1;
    if (census) {
      (brand_new ? census->fresh : census->swapped)++;
    }
    if (log && !brand_new && (swaps <= 3 || swaps == 10 || swaps == 100)) {
      REXLOG_INFO("ui interp event: {} kind=swap inst={:08X} d={} char={}->{} def={:08X}->{:08X} pos=({:.1f},{:.1f}) n={}",
                  step->name, inst, depth, e.charid, charid, e.def, def, live_m[4], live_m[5], swaps);
    }
    e = {};
    e.swaps = swaps;
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
    // How far this step moved the node (against the last step's value, not against what was shown).
    const double dx = double(live_m[4]) - e.to_m[4], dy = double(live_m[5]) - e.to_m[5];
    const double step_move = std::hypot(dx, dy);
    if (census) {
      census->gap += gap;
      census->moved += std::memcmp(live_m, e.to_m, sizeof live_m) != 0;
      census->cx += std::memcmp(live_c, e.to_c, sizeof live_c) != 0;
      census->max_move = std::max(census->max_move, step_move);
    }
    // Whether the step before this one moved it too, and the other way: the mark of a wrap (a ticker that has run
    // off one end and comes in at the other), which is a cut and not a movement.
    const bool followed = e.changed_serial && serial - e.changed_serial == 1 && !gap;
    const double prev_dx = e.last_dx, prev_dy = e.last_dy;
    const double last_move = std::hypot(prev_dx, prev_dy);
    const bool turned = followed && step_move > kInterpTurnPx && dx * prev_dx + dy * prev_dy < 0 &&
                        step_move > 8.0 * last_move;
    std::memcpy(e.from_m, gap ? live_m : e.shown_m, sizeof live_m);
    std::memcpy(e.from_c, gap ? live_c : e.shown_c, sizeof live_c);
    std::memcpy(e.to_m, live_m, sizeof live_m);
    std::memcpy(e.to_c, live_c, sizeof live_c);
    e.t0_us = step_us;
    e.changed_serial = serial;
    e.last_dx = dx;
    e.last_dy = dy;
    // A cut, a wrap or a flip is not a movement: shown at once.
    const double snap = double(REXCVAR_GET(mcla_ui_interp_snap_px));
    double scale_jump = 0;
    for (int k = 0; k < 4; k++) {
      scale_jump = std::max(scale_jump, std::fabs(double(e.to_m[k]) - double(e.from_m[k])));
    }
    const bool jumped = std::fabs(double(e.to_m[4]) - e.from_m[4]) > snap || std::fabs(double(e.to_m[5]) - e.from_m[5]) > snap;
    if (jumped || turned || scale_jump > 0.75) {
      std::memcpy(e.from_m, live_m, sizeof live_m);
      if (census) {
        census->snapped++;
      }
      if (log) {
        REXLOG_INFO("ui interp event: {} kind=snap why={} inst={:08X} d={} char={} step_move=({:.1f},{:.1f}) previous=({:.1f},{:.1f}) "
                    "scale_jump={:.2f}",
                    step->name, turned ? "turned" : jumped ? "far" : "flip", inst, depth, charid, dx, dy, prev_dx, prev_dy,
                    scale_jump);
      }
    }
  }
  e.serial = serial;
  e.seen_us = real_now;

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
                "world=({:.2f},{:.2f}) lm=({:.3f},{:.3f},{:.3f},{:.3f}) sm=({:.3f},{:.3f},{:.3f},{:.3f}) "
                "cx=({},{}|{},{}) applied={}",
                step->name, inst, depth, serial, t / 1000, t % 1000, alpha, live_m[4], live_m[5], e.shown_m[4], e.shown_m[5],
                LoadF32(inst + 48), LoadF32(inst + 52), live_m[0], live_m[1], live_m[2], live_m[3], e.shown_m[0], e.shown_m[1],
                e.shown_m[2], e.shown_m[3], int(live_c[0]), int(live_c[3]), int(e.shown_c[0]), int(e.shown_c[3]), apply ? 1 : 0);
  }
}

// The cost of the hook (mcla_ui_interp_stats): the time stamp counter around each call (a few nanoseconds; the
// steady clock would cost as much as the hook), turned into time by the ratio to the steady clock over each window.
inline uint64_t ReadTicks() {
#if defined(__x86_64__) || defined(_M_X64)
  return __builtin_ia32_rdtsc();
#else
  return NowMicros();
#endif
}
struct UiHookCost {
  uint64_t window_us = 0, window_ticks = 0;  // where the current window began
  uint64_t calls = 0, ticks = 0, worst = 0;  // in it: calls, total ticks, worst call
  uint64_t glide0 = 0, applied0 = 0;         // the self-check's counters at its start
};
UiHookCost g_hook_cost;
}  // namespace

void mcla_ui_interp_draw(PPCRegister& r30, PPCRegister& r31) {
  const bool apply_opt = REXCVAR_GET(mcla_ui_interpolate);
  const bool log = REXCVAR_GET(mcla_ui_interp_log);
  if (!REXCVAR_GET(mcla_ui_interp_stats)) {
    UiInterpDraw(r30, r31, apply_opt, log);
    return;
  }
  // The hook runs on one thread (the draw's), so the counters need no lock.
  const uint64_t t0 = ReadTicks();
  UiInterpDraw(r30, r31, apply_opt, log);
  const uint64_t t1 = ReadTicks();
  UiHookCost& c = g_hook_cost;
  c.calls++;
  c.ticks += t1 - t0;
  c.worst = std::max(c.worst, t1 - t0);
  if ((c.calls & 63) == 0 || !c.window_us) {
    const uint64_t now = NowMicros();
    if (!c.window_us) {
      c = {now, t1, 0, 0, 0, g_check_glide, g_check_applied};
    } else if (now - c.window_us >= 1'000'000 && t1 > c.window_ticks) {
      const double ns_per_tick = double(now - c.window_us) * 1000.0 / double(t1 - c.window_ticks);
      REXLOG_INFO("ui interp cost: option={} calls={} total_us={:.0f} per_call_ns={:.0f} worst_us={:.1f} gliding={} applied={}",
                  apply_opt ? "on" : "off", c.calls, double(c.ticks) * ns_per_tick / 1000.0,
                  c.calls ? double(c.ticks) * ns_per_tick / double(c.calls) : 0.0, double(c.worst) * ns_per_tick / 1000.0,
                  g_check_glide - c.glide0, g_check_applied - c.applied0);
      c = {now, t1, 0, 0, 0, g_check_glide, g_check_applied};
    }
  }
}

// Interpolated UI, the 3D side (mcla_ui_interp_camera; launcher/dev/ui-camera-2026-10-29.md). Besides the
// display list the pause menu moves a camera and 3D panels. Read from the recompiled game and the movie's
// script (not run before the check chain):
//  - The camera. The player context of every UI movie is of one class (vtable 0x820CACB8) whose advance,
//    sub_827238B0, steps the movie and then reads the script's `player0_cam_ctrl` (the blend between the game's
//    own view and the menu's, 0 to 1: the script moves it 3 percent of a sin/acos curve per step) and, when it is
//    not 0, builds a view matrix (sub_82723570, at ctx+496) from the camera the script keeps in `player0_cam`
//    (`position`, `lookat`, `fovy`, each eased toward the page's camera by 1/5 to 1/15 of the distance per
//    step). The UI manager's update (sub_821FC588) copies matrix, blend and field of view into itself (+4336,
//    +4200, +4404) and the game's camera (sub_8220AA68) mixes them with its own view in every update. The script's numbers change
//    only when the movie steps, so the camera moved 30 times a second whatever the frame rate.
//  - The panels. The page is drawn on a quad in the world: the script keeps a scene graph (`menu_group`,
//    `screen_mesh`, ...) whose `rot`, `scale`, `trans` (the page's size follows the list's by a third per step,
//    its place by a fifth) and `_alpha` the draw (sub_82724CF0 and what it calls) reads in every drawn frame.
// Everything is read through two small functions: one for an array of three numbers (sub_82722440) and one for
// a number (sub_822222F0). The hooks at the end of each blend what they read, by the same rule as the display list
// above (the step's value is reached one step period after the step; a cut is shown at once), but only inside the
// advance (between the hooks at 0x827238F8 and 0x82723934) and the scene draw (the hooks at the entry and at the
// end of sub_82724CF0): a script's own call into the game's camera (sub_822082D8) and everything else keeps what
// it read.
namespace {
// The swfCONTEXT whose script values this thread is reading for the camera or the panels; 0 anywhere else.
thread_local uint32_t t_scene_ctx = 0;
thread_local float t_scene_ctrl_live = 0.0f;  // the last blend value the script set, for the log

constexpr uint32_t Fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h = (h ^ uint32_t(uint8_t(*s++))) * 16777619u;
  }
  return h;
}

enum class SceneKind { kVec, kScalar, kCtrl };

struct SceneSpec {
  const char* name;
  uint32_t hash;
  SceneKind kind;
  double snap;  // a change longer than this (distance of the numbers) is a cut, shown at once
};
// What is blended, by the name the game asks for. The limits are far above one eased step of the script
// (a few tenths of a unit for the camera and the panels) and below its jumps (the 2D layout's camera is 46
// units away from the 3D one, and its field of view 25 degrees off); the blend value jumps by 1 for a cut.
constexpr SceneSpec kSceneSpecs[] = {
    {"position", Fnv1a("position"), SceneKind::kVec, 25.0},
    {"lookat", Fnv1a("lookat"), SceneKind::kVec, 25.0},
    {"trans", Fnv1a("trans"), SceneKind::kVec, 8.0},
    {"rot", Fnv1a("rot"), SceneKind::kVec, 1.2},
    {"scale", Fnv1a("scale"), SceneKind::kVec, 0.6},
    {"fovy", Fnv1a("fovy"), SceneKind::kScalar, 20.0},
    {"_alpha", Fnv1a("_alpha"), SceneKind::kScalar, 1.0e9},
    {"player0_cam_ctrl", Fnv1a("player0_cam_ctrl"), SceneKind::kCtrl, 0.5},
};

// The spec of the property name at a guest address (a constant string of the game), or null.
const SceneSpec* SceneSpecFor(uint32_t name_ptr) {
  if (name_ptr < 0x10000) {
    return nullptr;
  }
  const uint8_t* p = GuestBase() + name_ptr;
  char name[24];
  size_t n = 0;
  for (; n + 1 < sizeof name && p[n]; n++) {
    name[n] = char(p[n]);
  }
  if (p[n]) {
    return nullptr;  // longer than any name here
  }
  name[n] = 0;
  const uint32_t hash = Fnv1a(name);
  for (const SceneSpec& spec : kSceneSpecs) {
    if (spec.hash == hash && std::strcmp(spec.name, name) == 0) {
      return &spec;
    }
  }
  return nullptr;
}

const SceneSpec& CtrlSpec() {
  for (const SceneSpec& spec : kSceneSpecs) {
    if (spec.kind == SceneKind::kCtrl) {
      return spec;
    }
  }
  return kSceneSpecs[0];
}

struct UiSeries {
  uint32_t ctx = 0;
  int n = 0;
  uint32_t seen_serial = 0;  // the step serial at the last read
  uint64_t t0_us = 0;        // when the glide in progress started (the step it was seen at)
  uint64_t seen_us = 0;
  float from[3]{}, to[3]{}, shown[3]{};
};
std::unordered_map<uint64_t, UiSeries> g_series;  // under g_inst_mutex
uint64_t g_series_sweep_us = 0;
// Per movie slot: the step in which a change of the camera or the panels was last taken for a cut. Everything else that
// changes in that step is a cut too (the script switching between its 2D and 3D layouts moves the eye, the target, the field
// of view and the panels at once; blending only some of them would swing the picture for a step).
std::array<uint32_t, 32> g_scene_cut_serial{};

// Blends the n numbers (1 to 3) that the script gave for one property of one object of the context ctx, between
// its last two steps, with the rule of the display list. Returns true when v was changed to the blended value.
bool UiSceneBlend(uint32_t ctx, uint32_t obj, const SceneSpec& spec, float* v, int n) {
  const bool apply_opt = REXCVAR_GET(mcla_ui_interpolate) && REXCVAR_GET(mcla_ui_interp_camera);
  const bool log = REXCVAR_GET(mcla_ui_interp_log);
  if ((!apply_opt && !log) || OriginalTiming() || !REXCVAR_GET(mcla_ui_timing_fix) || ctx < 0x10000 || n < 1 || n > 3) {
    return false;
  }
  const UiStepState* step = nullptr;
  for (const UiStepState& s : g_ui_step) {
    if (s.context.load(std::memory_order_acquire) == ctx) {
      step = &s;
      break;
    }
  }
  if (!step) {
    return false;  // not a movie the step hook paces
  }
  const uint32_t serial = step->serial.load(std::memory_order_acquire);
  const uint32_t period_us = step->period_us.load(std::memory_order_relaxed);
  const uint64_t step_us = step->step_us.load(std::memory_order_relaxed);
  const uint32_t frame_us = step->frame_us.load(std::memory_order_relaxed);
  if (!serial || !period_us) {
    return false;
  }
  // As for the display list: nothing to gain with fewer than two frames per step.
  const bool apply = apply_opt && frame_us && uint64_t(frame_us) * 10 <= uint64_t(period_us) * 6;
  const uint32_t calls = step->calls.load(std::memory_order_relaxed);
  const uint64_t window_us =
      std::max<uint64_t>(1, uint64_t(period_us) * uint64_t(std::clamp(REXCVAR_GET(mcla_ui_interp_window_pct), 10, 100)) / 100);

  std::lock_guard lock(g_inst_mutex);
  UiFrameTime& frame_time = g_frame_time[size_t(step - g_ui_step.data()) % g_frame_time.size()];
  const uint64_t real_now = NowMicros();
  if (frame_time.calls != calls || real_now - frame_time.now_us > 6000) {
    frame_time = {calls, real_now};
  }
  const uint64_t now = frame_time.now_us;
  if (real_now - g_series_sweep_us > 2'000'000) {
    g_series_sweep_us = real_now;
    for (auto it = g_series.begin(); it != g_series.end();) {
      it = real_now - it->second.seen_us > 5'000'000 ? g_series.erase(it) : std::next(it);
    }
  }

  UiSeries& s = g_series[(uint64_t(obj) << 32) | spec.hash];
  const size_t bytes = size_t(n) * sizeof(float);
  // New, another movie's, or not read for a while (the menu was closed meanwhile): shown as it is.
  const bool fresh = s.ctx != ctx || s.n != n || real_now - s.seen_us > 200'000 || serial - s.seen_serial > 3;
  if (fresh) {
    s = {};
    s.ctx = ctx;
    s.n = n;
    std::memcpy(s.from, v, bytes);
    std::memcpy(s.to, v, bytes);
    std::memcpy(s.shown, v, bytes);
    s.t0_us = step_us;
  } else if (std::memcmp(v, s.to, bytes) != 0) {
    // A step changed it: glide from what was shown to the new value, unless steps went by without a read of it
    // or the change is a cut.
    double move2 = 0;
    for (int k = 0; k < n; k++) {
      const double d = double(v[k]) - double(s.to[k]);
      move2 += d * d;
    }
    const double move = std::sqrt(move2);
    const bool gap = serial - s.seen_serial > 1;
    uint32_t& cut_serial = g_scene_cut_serial[size_t(step - g_ui_step.data()) % g_scene_cut_serial.size()];
    const bool own_cut = move > spec.snap;
    const bool cut = own_cut || cut_serial == serial;
    if (own_cut) {
      cut_serial = serial;
    }
    std::memcpy(s.from, (gap || cut) ? v : s.shown, bytes);
    std::memcpy(s.to, v, bytes);
    s.t0_us = step_us;
    if (cut && log) {
      REXLOG_INFO("ui interp event: {} kind=snap why=scene3d what={} obj={:08X} move={:.3f} limit={:.3f} own={}", step->name, spec.name, obj,
                  move, spec.snap, own_cut ? 1 : 0);
    }
  }
  s.seen_serial = serial;
  s.seen_us = real_now;

  double alpha = 1.0;
  if (now < s.t0_us) {
    alpha = 0.0;
  } else if (now - s.t0_us < window_us) {
    alpha = double(now - s.t0_us) / double(window_us);
  }
  bool diff = false;
  for (int k = 0; k < n; k++) {
    s.shown[k] = alpha >= 1.0 ? s.to[k] : float(double(s.from[k]) + (double(s.to[k]) - s.from[k]) * alpha);
    diff |= s.shown[k] != v[k];
  }
  if (!diff) {
    return false;
  }
  if (log) {
    REXLOG_INFO("ui interp3d: {} {} ctx={:08X} obj={:08X} ser={} t={}.{:03} ms alpha={:.3f} live=({:.5f},{:.5f},{:.5f}) "
                "shown=({:.5f},{:.5f},{:.5f}) applied={}",
                step->name, spec.name, ctx, obj, serial, now / 1000, now % 1000, alpha, v[0], n > 1 ? v[1] : 0.0f,
                n > 2 ? v[2] : 0.0f, s.shown[0], n > 1 ? s.shown[1] : 0.0f, n > 2 ? s.shown[2] : 0.0f, apply ? 1 : 0);
  }
  if (!apply) {
    return false;
  }
  std::memcpy(v, s.shown, bytes);
  return true;
}
}  // namespace

// Entry of sub_82724CF0, the draw of a movie's 3D scene (r3 = its player context), and the instruction before its
// last jump (every way out goes through it).
void mcla_ui_scene_enter(PPCRegister& r3) {
  t_scene_ctx = r3.u32;
}

void mcla_ui_scene_leave(PPCRegister&) {
  t_scene_ctx = 0;
}

// End of sub_82722440 (an array of three numbers read from a script object: r30 = the object, r31 = where the
// numbers went, the property's name is the pointer saved at 96(r1)), where it succeeded.
void mcla_ui_scene_vec(PPCRegister& r1, PPCRegister& r30, PPCRegister& r31) {
  const uint32_t ctx = t_scene_ctx;
  const uint32_t out = r31.u32;
  if (!ctx || out < 0x10000) {
    return;
  }
  const SceneSpec* spec = SceneSpecFor(ReadGuestU32(r1.u32 + 96));
  if (!spec || spec->kind != SceneKind::kVec) {
    return;
  }
  float v[3] = {LoadF32(out), LoadF32(out + 4), LoadF32(out + 8)};
  if (UiSceneBlend(ctx, r30.u32, *spec, v, 3)) {
    StoreF32(out, v[0]);
    StoreF32(out + 4, v[1]);
    StoreF32(out + 8, v[2]);
  }
}

// End of sub_822222F0 (one number read from a script object; the name is saved at 80(r1)), where it succeeded.
void mcla_ui_scene_float(PPCRegister& r1, PPCRegister& r30, PPCRegister& r31) {
  const uint32_t ctx = t_scene_ctx;
  const uint32_t out = r31.u32;
  if (!ctx || out < 0x10000) {
    return;
  }
  const SceneSpec* spec = SceneSpecFor(ReadGuestU32(r1.u32 + 80));
  if (!spec || spec->kind != SceneKind::kScalar) {
    return;
  }
  float v = LoadF32(out);
  if (UiSceneBlend(ctx, r30.u32, *spec, &v, 1)) {
    StoreF32(out, v);
  }
}

// In sub_827238B0 right after `lfs f0,80(r1)`: f0 = the script's `player0_cam_ctrl` (r31 = the context). Its blend
// is the first thing of the camera's update; it also opens the window in which the reads above are blended. A blend
// value of 0 ends the update's camera work, so while the last step's glide to 0 is going on f0 is the glide's value.
void mcla_ui_cam_ctrl(PPCRegister& r31, PPCRegister& f0) {
  const uint32_t ctx = r31.u32;
  t_scene_ctx = ctx;
  float v = float(f0.f64);
  t_scene_ctrl_live = v;
  if (UiSceneBlend(ctx, ctx, CtrlSpec(), &v, 1)) {
    f0.f64 = double(v);
  }
}

// In sub_827238B0 before `ld r31,-16(r1)`, where every way out goes: r31 = the context. Closes the window and,
// with the log on, says what camera the update ended with.
void mcla_ui_cam_done(PPCRegister& r31) {
  t_scene_ctx = 0;
  const float live_ctrl = t_scene_ctrl_live;
  t_scene_ctrl_live = 0.0f;
  if (!REXCVAR_GET(mcla_ui_interp_log)) {
    return;
  }
  const uint32_t ctx = r31.u32;
  if (ctx < 0x10000) {
    return;
  }
  const float ctrl = LoadF32(ctx + 560);
  if (ctrl == 0.0f && live_ctrl == 0.0f) {
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
    return;
  }
  const uint64_t now = NowMicros();
  REXLOG_INFO("ui interp cam: {} ctx={:08X} ser={} t={}.{:03} ms ctrl={:.5f} live_ctrl={:.5f} fov={:.4f} "
              "m2=({:.5f},{:.5f},{:.5f}) m3=({:.5f},{:.5f},{:.5f})",
              step->name, ctx, step->serial.load(std::memory_order_relaxed), now / 1000, now % 1000, ctrl, live_ctrl,
              LoadF32(ctx + 568), LoadF32(ctx + 496 + 32), LoadF32(ctx + 496 + 36), LoadF32(ctx + 496 + 40), LoadF32(ctx + 496 + 48),
              LoadF32(ctx + 496 + 52), LoadF32(ctx + 496 + 56));
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

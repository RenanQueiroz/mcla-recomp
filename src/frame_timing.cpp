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
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

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
// With mcla_reflex_sleep_assist the update thread first waits on a high-resolution waitable
// timer until the margin (mcla_reflex_sleep_margin_us, 1.5 ms) before the moment the driver's
// wait would end, and then calls the driver as before, which spins only the last stretch.
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

REXCVAR_DEFINE_BOOL(mcla_reflex_sleep_assist, false, "MCLA",
                    "With NVIDIA Reflex holding the frame-rate cap on Direct3D 12 (Windows): the "
                    "update thread waits on a high-resolution timer until mcla_reflex_sleep_margin_us "
                    "before the moment the driver's wait would end, so the driver's busy wait "
                    "covers only the last stretch (6.4 to 6.9 ms of CPU a frame at a cap of 60 FPS "
                    "otherwise). Timing only. Off is the driver's own wait");
REXCVAR_DEFINE_INT32(mcla_reflex_sleep_margin_us, 1500, "MCLA",
                     "With mcla_reflex_sleep_assist: microseconds before the driver's expected "
                     "deadline at which the timer wait ends and the driver takes over. It covers "
                     "the timer's lateness (0.3 ms on average) and how far the driver's own "
                     "deadline is from the model; widened by itself when the driver's wait after "
                     "the timer comes back at once")
    .range(1000, 8000);

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
    const uint64_t margin = Margin();
    if (!engaged_) {
      engaged_ = true;
      stats_ = Stats();
      stats_since_us_ = now;
      REXLOG_INFO(
          "Reflex sleep assist: on at {} FPS (Direct3D 12): a timer wait until {} us before the "
          "driver's deadline (the previous start plus {} us), then NVIDIA Reflex's own wait",
          fps, margin, period);
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

  uint64_t Margin() const {
    const int64_t base = std::clamp<int64_t>(REXCVAR_GET(mcla_reflex_sleep_margin_us), 1000, 8000);
    return uint64_t(base + extra_us_);
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
    const int64_t base = std::clamp<int64_t>(REXCVAR_GET(mcla_reflex_sleep_margin_us), 1000, 8000);
    const uint64_t period = 1'000'000ull / uint64_t(fps);
    if (period < uint64_t(base) + 3000) {
      return "the cap's interval is too short for the margin";
    }
    if (period < uint64_t(base + extra_us_) + 3000) {
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
      REXLOG_INFO(
          "Reflex sleep assist: {} timer waits and {} frames with nothing to wait for in {:.1f} s at "
          "{} FPS: timer wait {:.0f} us on average, the driver's wait after it {:.0f} us, its "
          "return against the model (previous start plus interval) {:+.0f} us on average (from "
          "{:+d} to {:+d}), margin {} us (widened by {}), {} too tight, {} late wake-ups, {} timer "
          "errors",
          s.assisted, s.idle, double(now - stats_since_us_) / 1e6, fps,
          double(s.timer_wait_us) / double(s.assisted), double(s.driver_wait_us) / double(s.assisted),
          double(s.model_error_sum) / double(s.assisted), s.model_error_min, s.model_error_max,
          Margin(), extra_us_, s.tight, s.late, s.timer_errors);
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

namespace {
// The UI manager and its movie map (atMap name -> movie**: buckets at +60,
// count at +64; node: +0 name, +8 movie**, +12 next; movie: +56 player
// context). From LARecomp's notes, used with its author's permission.
constexpr uint32_t kUiManagerPtr = 0x8286D804;

uint32_t ReadGuestU32(uint32_t address) {
  if (address < 0x10000) {
    return 0;
  }
  const uint8_t* p = rex::Runtime::instance()->virtual_membase() + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool GuestStringIs(uint32_t address, const char* text) {
  if (address < 0x10000) {
    return false;
  }
  const char* s = reinterpret_cast<const char*>(rex::Runtime::instance()->virtual_membase() +
                                                address);
  return std::strncmp(s, text, std::strlen(text) + 1) == 0;
}

// Whether a player context belongs to one of the UI manager's screens other
// than the HUD map.
bool IsUiScreen(uint32_t context) {
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
        return !GuestStringIs(ReadGuestU32(node), "HUDMAPMOVIE");
      }
    }
  }
  return false;
}
}  // namespace

bool mcla_ui_movie_advance(PPCRegister& r25, PPCRegister& r31) {
  if (OriginalTiming() || !REXCVAR_GET(mcla_ui_timing_fix) || r25.u32 == 0 ||
      !g_engine_timer_started.load(std::memory_order_relaxed)) {
    return false;
  }
  constexpr uint64_t kScreenPeriodMicros = 1'000'000ull / 30;
  struct Schedule {
    uint32_t context;
    uint64_t next_us;
    uint64_t checked_us;  // when IsUiScreen was last asked: movies come and go
    bool screen;
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
    *schedule = {context, 0, 0, false};
  }
  if (now - schedule->checked_us > 1'000'000) {
    schedule->checked_us = now;
    schedule->screen = IsUiScreen(context);
  }
  uint64_t period_us = kScreenPeriodMicros;
  if (!schedule->screen) {
    const int32_t hud_rate = REXCVAR_GET(mcla_ui_hud_rate);
    if (hud_rate <= 0) {
      return false;
    }
    period_us = 1'000'000ull / uint64_t(hud_rate);
  }
  if (schedule->next_us && now + period_us / 4 < schedule->next_us) {
    return true;
  }
  const bool behind = !schedule->next_us || now > schedule->next_us + period_us;
  schedule->next_us = behind ? now + period_us : schedule->next_us + period_us;
  return false;
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

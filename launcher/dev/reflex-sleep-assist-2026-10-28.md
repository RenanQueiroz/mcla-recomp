# The Reflex sleep assist (2026-10-28)

Branch `cpu/reflex-sleep-assist`. Options `mcla_reflex_sleep_assist` (off by default) and `mcla_reflex_sleep_margin_us` (1500).
**Status: written and compiled by CI; not run in the game, nothing below is measured.** Every number in the "Why" section comes
from earlier profiles of the shipped game (1 kHz sampling, not controlled A/B runs).

## Why

With NVIDIA Reflex on (its default on NVIDIA) and a frame-rate cap, the driver holds the cap: `LowLatency::OnFrameStart` calls
`NvAPI_D3D_Sleep` at the top of the engine timer, and the minimum frame interval it was given is the cap's. On Direct3D 12 that
call is a busy wait. The profile of the update thread (the thread that runs the engine timer) shows `RtlQueryPerformanceCounter`,
frames of the driver's `nvwgf2umx.dll` and short sleeps (`ZwDelayExecution`) for the whole wait: 1.2 to 1.5 ms of CPU a swap
uncapped, 6.4 to 6.9 ms at a cap of 60 FPS and about 15 ms at 30 (the cpu3 and hand-off analyses of this round, which are not in the
repository). On Vulkan the same call is a semaphore wait (`vkLatencySleepNV`) and costs no CPU. At the game's default cap of 30 FPS on
a 4-core CPU that is estimated at 0.4 to 0.5 of a core.

## What the option does

Windows, Direct3D 12, NVIDIA Reflex holding the cap only. In the frame-start hook (`mcla_frame_start`, `src/frame_timing.cpp`), before
`LowLatency::OnFrameStart`, the update thread waits on a high-resolution waitable timer
(`CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) until `mcla_reflex_sleep_margin_us` before the moment the
driver's wait would end; the driver's own call follows and spins only the last stretch. Nothing the game computes changes: the
engine timer reads the time after the wait as before, and the simulation step is the elapsed time. It moves where the wait is
spent. The only timing change is a start later than the driver's choice when the model is wrong (below).

### The deadline: what is known and what is a model

Known: NVIDIA documents `minimumIntervalUs` as the "minimum frame interval in microseconds, 0 = no frame rate limit" and
`NvAPI_D3D_Sleep` as the entry point where the driver sleeps "due to frame rate limit and/or low latency features"
(nvapi.h, release 590). For Vulkan the same field is "the microseconds between vkQueuePresentKHR calls for a given swapchain that
vkLatencySleepNV will enforce" (Vulkan reference for `VkLatencySleepModeInfoNV`). Not documented: between which two events the
NVAPI interval is measured, and how much the latency reduction moves the end of the wait. The wait is therefore a model:

- deadline = the time the previous `OnFrameStart` returned + 1,000,000 / cap (the same integer division the driver is told);
- used only while `LowLatency::DriverLimits()` is true (the mode was taken, an interval is set and the last wait was made), the cap
  is above 0, the API is Direct3D 12 (`MclaGraphicsIsVulkan()`), the 16 passes before ran at the same cap, and the cap's interval
  leaves room for the margin;
- not used uncapped (without an interval the driver's pacing has no deadline that can be known: its wait there is 1.2 to 1.5 ms,
  no longer than the margin), with `low_latency_limiter=fork`, with frame generation, on AMD (Anti-Lag 2 is untested and its
  limiter is the game's), on Vulkan, on Linux and the PS5 (not built or run there);
- checked every frame it is used for. After the timer the driver's call is timed. A call that returns in under 200 us although the
  model gave it at least 700 us is "too tight": the driver's deadline is earlier than the model's and the timer may have delayed the
  frame. The margin is widened by 0.5 ms (at most 4 ms) and narrows by 0.125 ms after 64 frames without one. Eight troubles (too
  tight, or a timer that woke over 1 ms late) among the last 32 frames switch the assist off for 10 s, after which the 16 passes
  and the whole check start again;
- a frame whose own work already used the time until the deadline minus the margin makes no timer wait.

The log (`game.log`): `Reflex sleep assist: on at N FPS ...` once when it starts running; `not running: <reason>` once per reason
that lasts 3 s (Vulkan, no Reflex, no cap, limiter fork, frame generation, no timer); every 10 s while it runs a line with the number
of timer waits, the average timer wait, the average wait left to the driver, the driver's return against the model (mean, min,
max), the margin, and the counts of too-tight frames and late wake-ups; a warning when it switches itself off.

## The alternative: the game's own limiter holds the cap

`low_latency_limiter` (SDK option, `patches/rexglue-low-latency.patch`): `auto` (the default: the driver where Reflex has been
tested and frame generation is off), `driver`, `fork`. With `fork` the game's limiter (`WaitForFrameDeadline`: a timer sleep, then a
1.5 ms spin) holds the cap at the end of the frame and Reflex's wait is short because the interval it was given is 0. That also
takes the driver's busy wait out, at the price that the input is read earlier in the frame than with the driver's cap
(`KNOWN-ISSUES.md`: a 60 FPS cap shows the input 8.2 ms newer with the driver, 45 FPS 13.6 ms). It needs no code; the chain
compares it with the assist.

## How it is to be measured

One exclusive hold of at most 40 minutes (a chain script kept outside the repository): the build of this branch, then Direct3D 12, RTX 5080, native
resolution, HDR off, the pad `launcher/dev/scripts/reflex-ab.pad` (standing in the city: uncapped 8 s, then a cap of 60 for 10 s,
then 30 for 12 s, set during the run), at 16 logical processors and at 4 (affinity 0xF), three variants (off, assist,
`low_latency_limiter=fork`), three runs each per logical count in the order A B C C B A A B C, plus a warm-up run first.

Per run and segment: the process' CPU per swap and the Main thread's (`threads.csv`, 1 Hz, interpolated to the marks); the frame
median and 99th percentile, the swap interval's standard deviation and the share of intervals more than 1 ms off the cap's period
(`frames.csv`); PresentMon's present spacing and the engine step against the display interval, sigma (`presents.py`); the audio
counters. **Latency** (the point of Reflex): from the engine timer pass that read a frame's input to the frame on the screen,
median and 99th percentile (`presents.py`, PresentMon's `DisplayLatency`), and the driver's own report of input sample to GPU render
end and to present end (`low_latency_debug=2`, logged every 300 frames); the assist adds no input latency if both equal the off
variant's within their spread. The wait the driver still makes (`the wait before the frame averaged ...` in the same log line)
shows directly how much of the wait the timer took over.

Rules (the report script of that chain; the spread of a metric is the range of the off runs):

| Check | PASS | FAIL |
|---|---|---|
| CPU per swap at a cap | at least 5% and 2 spreads less than off | more than 3% and 2 spreads more |
| the cap held | swaps a second within 1% of 60 / 30 | otherwise |
| uncapped frame median / p99 | at most +1% / +3% against off | otherwise |
| swap interval sigma, share >1 ms off the period | not above off by more than 0.1 ms / 0.5 points or 2 spreads | above by twice that |
| start-to-screen latency, driver's input-to-present | assist not above off by more than 0.3 ms or 2 spreads | above that |
| the assist engaged | log line `on at` and timer waits in the cap segments | else LOOK (reason in the log) |
| the model | no warning that it switched itself off; too tight under 1% of frames | otherwise |

LOOK is anything between (no CPU benefit, borderline pacing). The fork limiter's latency is reported, not judged: it is later by
design.

## Results

Not run yet. This section is to be filled from the chain's report.

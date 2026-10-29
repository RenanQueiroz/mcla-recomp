# The Reflex sleep assist's margin (2026-10-28)

Branch `cpu/reflex-assist-margin`: `integrate/v0.3.0` (6472cdb) + `cpu/reflex-sleep-assist` (a0c15e5) + the options below. On the branch the assist was off by default; **in v0.3.0 it is on, with `mcla_reflex_sleep_margin_pct` 20** (decided after the sweep, see "Results").
Added: `mcla_reflex_sleep_margin_pct` (0 to 80, default 0 = off on the branch, 20 in v0.3.0) and `mcla_reflex_sleep_margin_sum` (default off). Timing only; Vulkan, AMD, Linux and the PS5 build are untouched
(the code is Windows and Direct3D 12 only, as before).
**Status: the sweep in "The sweep" was run (chain `reflexmargin`, 2026-10-28, branch head 26a38e7); its numbers are in "Results".** Every number under "Where the first measurement left off"
comes from the first chain (`reflexsleep`, 2026-10-28), with a fixed margin of 1.5 ms and without the frame-cap timer.

## Where the first measurement left off

The first chain (RTX 5080, Direct3D 12, native resolution, HDR off, the city, 3 runs per cell, margin fixed at 1.5 ms; table in `reflex-sleep-assist-2026-10-28.md`) found:

- the assist engages as modelled: the driver's call after the timer returned on the model's deadline (+0 us on average) and 0 of 5,409 timer waits were too tight;
- the process's CPU per swap at a cap of 60 / 30 FPS fell by 26% / 44% at 4 logical processors and by 19% / 26% at 16;
- PresentMon's start-to-screen latency did not change (+0.02 to +0.15 ms at the caps);
- the pacing was a little less even at the caps: the swap interval's sigma +0.02 to +0.12 ms, display frames more than 1 ms off the period +1.2 to +3.4 points, the 99th
  percentile the same. At 16 logical processors, 30 FPS, the display share was outside the noise of the `off` runs (4.6 against 1.1 points; the allowed difference was 1.7).
- The "unexplained" rise of the swap interval's sigma at 4 logical processors, uncapped (1.16 to 2.47 ms) is one frame: a swap interval of 176.7 ms in run `a0xF-assist-r1`
  (without it that run's sigma is 1.14 ms; the other two assisted runs had 1.07 and 1.16 ms against 1.14 to 1.17 for `off`). The assist does nothing uncapped, and no other interval over
  50 ms occurred in the other 50 segments. The cause of that frame is not known. The sweep repeats the comparison (below).

What is not known is why the pacing is less even. Two candidates, neither tested:

1. The timer wake-up is late (0.28 ms on average here: the driver was left 1.22 ms of the 1.5 ms margin), and a late wake-up has a tail; the margin is also what absorbs a model that is
   off. A larger margin leaves more room for both.
2. After a long timer wait the core may have lowered its clock or entered an idle state, which a busy wait does not allow; the first instructions of the frame then run slower.
   A longer spin before the frame would shorten that.

Precedent, read on 2026-10-28 and not checked again here: DXVK's latency sleep (`src/util/util_sleep.cpp` in `doitsujin/dxvk`) sleeps with `NtDelayExecution` at the timer's best
resolution and then busy-waits the last 4 timer granularities plus a sixth of the wait: about 4.5 ms at 60 FPS and 7 ms at 30 (`dxvk.latencyTolerance`, default 1000 us, "higher values
increase latency, but may lead to better frame pacing"). RTSS's limiter is reported to sleep for a share of the period (reports say 70 to 90%) and busy-wait the rest, with users reporting that
passive waiting can make frame times less stable (forum reports, not measured here). A HR waitable timer is reported to be 155 us late on average and up to about 730 us (the `spin_sleep`
crate's notes, not measured here). A fixed 1.5 ms is much tighter than both.

## The options

| Option | Default | Meaning |
|---|---|---|
| `mcla_reflex_sleep_assist` | on (v0.3.0; off on the branch) | The assist (see `reflex-sleep-assist-2026-10-28.md`). |
| `mcla_reflex_sleep_margin_us` | 1500 (1000 to 8000) | The fixed part of the margin: microseconds before the driver's modelled deadline at which the timer wait ends. |
| `mcla_reflex_sleep_margin_pct` | 20 in v0.3.0 (0 to 80; 0 on the branch) | A share of the cap's frame interval (1,000,000 / cap, as the driver is told it), in percent. 0 is off. |
| `mcla_reflex_sleep_margin_sum` | off | With a share above 0: the margin is the fixed part plus the share, not the larger of the two. |

The margin is computed in every frame, so `set` in a pad or a console takes effect at once. The self-check works on top of whatever the settings give: a driver call that returns at once
although the model gave it at least 0.7 ms widens the margin by 0.5 ms (up to 4 ms; 0.125 ms narrower after 64 frames without it), and 8 troubles (that, or a timer that woke over 1 ms late)
among the last 32 frames switch the assist off for 10 s. A cap whose interval is under the margin plus 3 ms does not use the assist (the log says so once), and a margin widened past that
starts again from the settings after 10 s. The log line when it starts running says the rule (`margin rule: the larger of 1500 us and 20.0% of 16666 us`) and the 5-second lines give the
margin in use.

| Rule | `margin_us` | `margin_pct` | `margin_sum` | at 60 FPS | at 30 FPS |
|---|---|---|---|---|---|
| fixed (the first measurement) | 1500 | 0 | off | 1,500 us | 1,500 us |
| 10% | 1500 | 10 | off | 1,667 us | 3,333 us |
| 20% | 1500 | 20 | off | 3,333 us | 6,667 us |
| DXVK-like (1 ms + 17%, about a sixth) | 1000 | 17 | on | 3,833 us | 6,667 us |

At 30 FPS the 20% rule and the DXVK-like rule give the same margin, and at 60 FPS the fixed rule and 10% differ by 0.17 ms: those pairs are replicates of one setting, which gives a
second estimate of the noise.

## The sweep (run once, 2026-10-28)

One exclusive hold of at most 45 minutes (a chain script kept outside the repository), build of this branch included, D3D12, RTX 5080, native resolution, HDR off, every run with
`--mcla_timer_hr_at_cap=true` (the frame-cap timer is on by default in v0.3.0, so this measures on top of it), `--low_latency_debug=2`, PresentMon and the present log on. Logical
processors: 4 (affinity 0xF) and 16.

- The five settings (`off`, 1.5 ms fixed, 10%, 20%, DXVK-like) are switched inside each run with the pad's `set` (the options are read every frame; `mcla_poll_wait` was measured that
  way too): one boot serves all five at both caps. In `launcher/dev/scripts/reflex-margin-r1.pad` to `-r3.pad` each variant gets 2.5 s of settle (the assist is turned off for 0.3 s, the
  options set, the assist turned on; it needs 16 frames at one cap before the first timer wait) and then 10 s at 60 FPS or 12 s at 30 FPS, between marks `c60_<variant>_a` and `_b`. The
  order rotates over the three runs (rotations 0, 2 and 4 of off, 1.5 ms, 10%, 20%, DXVK-like) and the 30 FPS stretch runs the reverse of the 60 FPS one, so no setting is always first
  or last. Before the caps and after them there is an uncapped stretch of 8 s (assist off, then assist on with the DXVK-like margin): the control, where the assist is not engaged.
  Three runs per logical count, so 3 samples per setting, cap and count.
- A block of ten 4-logical uncapped-only runs (`reflex-uncapped.pad`, 12 s uncapped; the assist off or `--mcla_reflex_sleep_assist=true` on the command line, order A B B A A B B A A B, so 5
  runs a side) to see whether the swap interval's sigma differs with the assist on. A warm-up run first (`reflex-warm.pad`: 60 FPS with the assist on at the DXVK-like margin, set at run time the way the
  sweep sets it; the chain stops if the log lacks the "Reflex sleep assist: on at 60 FPS" line with `margin rule: 1000 us plus 17.0% of 16666 us`, which also proves that the four `set`
  commands and the margin formula work in the built exe).
- Estimated: about 36 minutes of runs (a sweep run about 225 s, an uncapped run about 77 s, each with about 10 s of display and fingerprint checks, as the first chain's runs had) plus the
  build (the first chain's took 2.7 minutes) and the checks. A run is not started if it would end after the steps' deadline, so a slow build drops the last uncapped runs first.
- Measured per stretch: the process's and the Main thread's CPU per swap (1 Hz thread log interpolated to the marks), the frame median and 99th percentile, the swap interval's sigma (and the
  sigma without hitches, which are intervals over 50 ms or over 3 frame periods, whichever is longer; the number of hitches; the longest interval), the share of intervals more than 1 ms off the period, PresentMon's display sigma, its share more than 1 ms
  off and the judder (engine step against display interval), PresentMon's start-to-screen latency (median and 99th percentile), the driver's wait before the frame and the assist's own log
  lines. The driver's own "input to present" latency is reported as information only (its baseline is negative when the driver holds the cap, so it is not a measure).
- Rules, per logical count and cap, each setting against `off` (the spread is the range of the three `off` stretches): the sigmas and the judder PASS if not above `off` by more than
  max(0.10 ms, 2 spreads), LOOK up to twice that, FAIL above; the shares more than 1 ms off the period max(0.5 points, 2 spreads); the 99th percentile max(1 ms, 2 spreads); the start-to-screen
  latency median max(0.3 ms, 2 spreads), its 99th percentile max(1 ms, 2 spreads). PresentMon's display sigma, share and judder leave the hitches out too. A setting with 3 or more hitches than `off` in total gets a LOOK. A setting is "within the noise of `off`" when every
  cap, pacing, latency, engaged and model check PASSes (the CPU change is not part of it). The report names,
  for each cap and count, the smallest margin that is, and its CPU saving (percent and ms per swap, and the share of the fixed 1.5 ms setting's saving that it keeps). If none is, it says so and
  names the smallest margin with no FAIL.
- The assist's own checks: the log shows it engaged in every stretch (timer waits in the 5-second lines at the stretch's margin), under 1% of the timer waits too tight, no "do not fit the model"
  warning inside a stretch, the audio's steady underruns not above `off`.

What this cannot show: a difference smaller than the spread of three samples; whether a setting that passes here passes in other scenes (one place in the city, standing still); AMD,
Vulkan, Linux, the PS5 build; the exactness of the game's timing (the recipe is a `--mcla_fixed_dt` comparison of `off` against the assist, not done); a live switch is not the same as a launch
option for state that lives across the switch (the pad resets the assist's state by turning it off for 0.3 s; the margin widening starts again from nothing at each switch).

## Results

Chain `reflexmargin` (2026-10-28), exe built from 26a38e7 (mcla.exe sha256 5723623...), 16 of 16 measured runs valid, protected files unchanged. RTX 5080, Direct3D 12, native resolution, HDR off, windowed, the city (standing still),
`--mcla_timer_hr_at_cap=true`, a Ryzen 7 9800X3D with the process limited by affinity to 4 logical processors (0xF) and to all 16. Three runs per logical count; the five settings were switched inside each run and each is compared with the `off`
stretches of the same runs (paired). No steady audio underrun in any run. The chain's report is not in the repository.

The default of v0.3.0 is the `20%` setting (margin = the larger of 1,500 us and 20% of the cap's interval: 3,333 us at 60 FPS, 6,667 us at 30). `off` against `20%`, mean of the 3 runs:

| | 4 logical, 60 FPS | 4 logical, 30 FPS | 16 logical, 60 FPS | 16 logical, 30 FPS |
|---|---|---|---|---|
| process CPU per swap, off to 20% (ms) | 20.42 to 13.79 (-32.5%) | 30.11 to 15.88 (-47.3%) | 18.85 to 13.96 (-26.0%) | 29.75 to 14.50 (-51.3%) |
| the same with the fixed 1.5 ms (ms) | 14.39 (-29.5%) | 15.85 (-47.3%) | 12.56 (-33.4%) | 14.71 (-50.6%) |
| noise of the `off` runs (range over mean) | 5.0% | 15.8% | 2.9% | 8.9% |
| main thread CPU per swap (ms) | 8.88 to 3.09 | 17.85 to 3.36 | 8.90 to 2.90 | 17.28 to 2.99 |
| swap interval sigma without hitches (ms) | 0.283 to 0.231 | 0.233 to 0.281 | 0.152 to 0.158 | 0.197 to 0.183 |
| swap interval 99th percentile (ms) | 17.31 to 17.23 | 33.91 to 33.98 | 17.13 to 17.13 | 33.91 to 33.81 |
| display frames more than 1 ms off the period (%) | 6.23 to 4.79 | 1.30 to 2.69 | 3.78 to 4.12 | 1.39 to 0.74 |
| PresentMon display sigma (ms) | 0.496 to 0.422 | 0.373 to 0.452 | 0.408 to 0.413 | 0.339 to 0.333 |
| start-to-screen latency median (ms) | 9.21 to 9.08 | 8.78 to 9.44 | 8.72 to 8.82 | 9.05 to 9.11 |
| start-to-screen latency 99th percentile (ms) | 10.77 to 10.54 | 10.45 to 10.43 | 10.26 to 10.29 | 10.17 to 10.03 |
| the driver's wait before the frame (ms) | 13.96 to 3.08 | 23.29 to 6.99 | 13.77 to 3.30 | 22.16 to 6.94 |
| timer waits too tight / late wake-ups | 0 of 1,798 / 2 | 0 of 901 / 0 | 0 of 1,803 / 2 | 0 of 901 / 2 |
| the chain's verdict | PASS within the noise | PASS within the noise | PASS within the noise | PASS within the noise |

- Every pacing check of the chain (swap interval sigma, swap intervals and display frames more than 1 ms off the period, PresentMon's display sigma and judder, the swap 99th percentile) was within the allowed difference, which is the larger of a fixed floor
  and two ranges of the `off` runs, in all four cases; the latency checks too (the allowed difference of the median at 4 logical processors and 30 FPS was 4.42 ms because the three `off` runs spread over 2.21 ms; the measured difference was +0.66 ms).
  With n=3 these checks cannot show a difference smaller than the runs' own spread.
- The other settings, against `off`: 1.5 ms fixed was within the noise at 60 FPS (both counts) and at 16 logical processors and 30 FPS and got a pacing LOOK at 4 logical processors and 30 FPS (swap intervals more than 1 ms off the period 0.74%
  against 0.00%, allowed 0.5 points); 10% (1,667 us at 60, 3,333 us at 30) got the same LOOK, with the swap interval sigma +0.142 ms against an allowed 0.135 as well; 1 ms plus 17% (3,833 us at 60, 6,667 us at 30) got the LOOK at 4 logical processors and
  30 FPS only. 20% is the only setting within the noise in all four cases. At 30 FPS the 20% and the 1 ms plus 17% rules are the same margin (6,667 us), a replicate that shows the noise between runs of one setting: the two had different verdicts at 4
  logical processors, so a LOOK of this chain is a small effect between small samples.
- The CPU saving: at 60 FPS 20% keeps less of it than 1.5 ms at 16 logical processors (-26.0% against -33.4%); at 4 logical processors and at 30 FPS it is the same as 1.5 ms within the spread. The chain's own choice per case was 1.5 ms where that was within the noise
  and 20% at 4 logical processors and 30 FPS; the single setting that fits all four, 20%, was chosen for the default.
- **Uncapped** (the assist is not engaged: the log says so): in the sweep runs (the assist on with the DXVK-like margin against off) the frame median was +0.8% (4 logical) and -0.1% (16), the 99th percentile -1.9% and -0.7%. The block of ten 4-logical uncapped runs (5 a side, order A B B A A B B A A B,
  12 s each): frame median 7.037 (assist) against 7.045 ms (off), 99th percentile 9.51 against 9.49 ms, swap interval sigma 1.038 against 1.055 ms, no hitch (a swap interval over 50 ms or over 3 periods) in any run, the longest interval 11.3 against 11.9 ms.
  The odd frame of the first chain (176.7 ms, run `a0xF-assist-r1`) did not come back in these 10 runs nor in the 6 sweep runs; its cause is still unknown.
- **Not tested:** a cap of 120 FPS (only 30 and 60); other NVIDIA GPUs and drivers; a slower CPU (the test PC's 4 logical processors are the first of a fast CPU); other scenes (one place in the city, standing still); the exactness of the game's timing (the recipe
  is a `--mcla_fixed_dt` comparison of the assist against off, not done); the assist set by a launch option (here: switched inside the runs, with the assist turned off for 0.3 s at each switch and the margin's widening restarting); Vulkan, AMD and Intel
  (the assist is not used there); Linux and the PS5 build (not built or run there). The pads in `launcher/dev/scripts/reflex-*.pad` set the assist and the margin themselves; with the assist on by default, a run that needs it off passes `--mcla_reflex_sleep_assist=false`.

### History: the first measurement and its pacing LOOKs

The first chain (`reflexsleep`, 2026-10-28, fixed margin of 1.5 ms, without the frame-cap timer, 3 runs a side, same PC and route; `reflex-sleep-assist-2026-10-28.md`) saved 26% / 44% (4 logical, caps 60 / 30) and 19% / 26% (16 logical) of the CPU per swap, left
the start-to-screen latency unchanged (+0.02 to +0.15 ms) and paced a little less evenly (swap interval sigma +0.02 to +0.12 ms; display frames more than 1 ms off the period +1.2 to +3.4 points; at 16 logical processors and 30 FPS the display share, 4.6 against 1.1 points,
was outside the noise of the `off` runs, allowed difference 1.7). Its "driver input-to-present" FAILs were a broken metric (a negative baseline). Those pacing LOOKs are why the margin became a share of the interval. The sweep above is the answer on the same PC with the frame-cap
timer on: the larger margin removes the LOOKs and keeps the saving, within the small samples.

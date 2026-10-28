# CPU profiling, 2026-10-26

Windows 11, Ryzen 7 9800X3D (8 cores, 16 threads), RTX 5080, the fork at v0.1.2 (`09c67b3`) built with llvm-mingw
(`-Toolchain mingw`, the prebuilt SDK, the launcher's PC defaults), native resolution, uncapped (`--mcla_fps=0`),
`--mcla_intro=normal`, the dev pad's routes. The profiler is the one in `src/sampling_profiler.cpp`
(`scripts/profile_report.py` reads it); this file replaces the CPU half of `profiling-2026-10-14.md`, whose
"GPU-fence spin in `sub_82788F80`, 8%" was a misattribution: the old tool took any pc after the last generated function
for that function, and the host code that follows it is not guest code.

## Method

- `--mcla_profile=<file> --mcla_profile_delay=-1 --mcla_profile_seconds=15 --mcla_profile_hz=1000` and, in the pad
  script, `set mcla_profile_segment <label>`: a window of 15 s starts and is written to `<file stem>.<label><ext>`.
  Every thread is suspended in turn (about 670 rounds a second; a thread that was blocked in ntdll and has used less
  than 150,000 cycles since is counted again without being suspended), its context read and its host stack walked with
  the PE unwind tables. A frame is a guest function (`sub_XXXXXXXX`) only if the host function that contains it, found in
  the exe's `.pdata`, is a dispatch table entry. The window also holds each thread's CPU time, the swaps and engine
  frames, and a per-round timeline of the ten busiest threads. Sampling costs 3 to 4% of the frame rate (145 to 140 FPS).
- `--mcla_profile_thread_log=<csv>`: every thread's CPU time once a second, for the whole run, at no cost (the figures
  of CPU per swap below come from it, from stretches without sampling).
- `launcher/dev/scripts/cpup-roam.pad` (title, map zoom-out, 8 s of free roam, a 15 s window standing at the spawn point
  and a 15 s window driving out of the car park and on) and `cpup-race.pad` (the race benchmark of `race.pad`, a 15 s
  window from 10 s of game time after the start, `--mcla_fixed_dt 60`). The races and the Vulkan runs were done after a warm-up of the
  same route (the D3D12 city route once, the scratch shader cache being warm); the display was woken before and
  checked after (`fgdisplay.py`): on at both ends of all four.
- Reports: `scripts/profile_report.py <window> [--joint --strip N] [--callers sub_XXXXXXXX]`.

## Where the time goes (per swap, from the thread log)

| Scene (D3D12) | Frame | Process CPU | TimerQueue | Main guest thread | Render guest thread | GPU command thread | GPU submission |
|---|---|---|---|---|---|---|---|
| Free roam | 6.82 ms (146 FPS) | 23.2 ms = 3.4 cores | 6.86 | 6.46 | 3.83 | 3.31 | 1.39 |
| Race | 7.03 ms (142 FPS) | 37.3 ms = 5.3 cores | 7.11 | 5.33 | 5.97 | 4.90 | 1.7 (sampled window) |

Vulkan is the same within 5% in each thread (free roam 6.77 ms, race 7.13 ms).

1. **The timer queue thread uses a whole core in every scene** (`rex::thread::TimerQueue`, 98 to 99% of a core,
   91 to 93% of its samples in `RtlSleepConditionVariableSRW`). Its only timer is the 1 ms one that updates the guest's
   `KeTimeStampBundle`; libc++'s `condition_variable::wait_until` on Windows cuts the time left down to whole
   milliseconds, so a deadline under 1 ms away returns at once and the thread goes round until it is due. See
   `patches/README.md`, `rexglue-timer-queue-blocking-wait.patch`.
2. **The main guest thread spends 40% of its time polling**: `sub_82148A10`, a ring of three buffer slots per thread class,
   waits for the next slot to be freed (by the next round of the thread that runs `sub_82144888`, see "The wait for a ring
   slot" below; it is not the render thread) by calling guest `Sleep(0)` (`sub_82460270` ->
   `KeDelayExecutionThread` -> `XThread::Delay(0)` -> `SwitchToThread()`), reading the clock twice a turn
   (`sub_82144FC8`, `QueryPerformanceCounter`). 2.8 ms of its 6.5 ms CPU per swap in free roam, 0.9 ms in the race;
   about 30% of its CPU time is kernel time.
3. **Update and render are close to serialized.** In free roam the main thread runs while the render thread waits for
   work (`sub_82135DC0`, a semaphore wait) 35% of the time, the render thread runs while the main thread polls 27%, both
   run 25%, both wait 9%. With full overlap a frame would take roughly max(3.6, 3.9) = 4 ms; it takes 6.9. In the race the
   render thread (6.1 ms of CPU per swap, 84% of a core) is the limit. Hand-off figures need a trace of the hand-off
   points, not a sampler.
4. **The guest code is flat.** No guest function is over 5% of its thread (render thread: `sub_82422160` 5 to 6%,
   `sub_822DF960` 2 to 4%, `sub_8241D620` 2%; main thread: nothing over 1% apart from the polling). `ucrtbase.dll!fma`
   (a call for every fused multiply-add, the SDK being built for x86-64-v2) is 0.7 to 1.8% of the job and render
   threads.
5. **`sub_82422160` and the Interlocked helpers hold the SDK's global lock.** 33 guest functions (45 sites) are the
   360's `InterlockedIncrement` and friends, `mfmsr`/`mtmsrd` around a `lwarx`/`stwcx.` loop, which the recompiler turns
   into `REX_CHECK_GLOBAL_LOCK` / `REX_ENTER_GLOBAL_LOCK` / `REX_LEAVE_GLOBAL_LOCK` (a recursive mutex shared by every
   thread and the SDK) around a compare-and-swap. Every one of the 45 regions holds nothing but loads, a compare and the
   compare-and-swap. They cost the render thread 0.46 to 0.58 ms per swap in free roam (6 to 8% of its samples) and
   0.82 ms in the race (11%), the main thread 0.04 ms.
6. **The scripted pad costs the main thread 0.24 ms per swap in the race** (`VirtualQuery` in `PlayerPosition` and
   `TipsStep`, from `MclaDevPadFrame`): a measurement tool's cost, in every run done with a script.

The generated code switches the SSE flush mode 4,836 times by `ldmxcsr` (`enableFlushModeUnconditional` 2,657 sites,
`disableFlushModeUnconditional` 2,179) and checks it at about 32,000 more; the profile cannot see an instruction, so its
share has to be measured by an A/B with the instruction removed (not done at the time of writing).

## The timer queue fix, checked in the game (2026-10-27)

A build of the SDK with counters in the timer thread (dumped every 5 s), one 65 s run of the free-roam route, D3D12, native, uncapped,
16 logical processors: every wait for a timer due within 20 ms (63,040 of them) ended on the high-resolution timer, none on the
event for a newly queued timer, none fell back to the condition variable, the timer was made; 1,000 callbacks a second; a wait
was 0.33 ms late on average (0.69 ms asked; the worst, 33 ms, while the game was loading). The `rex::thread::TimerQueue` thread
used 0.04 ms of CPU per swap in free roam (6.87 without), the whole process 17.7 ms per swap (23.5), and the frame median was
6.85 ms in both.

The alternating comparison, D3D12, native, uncapped, the same route (free roam, standing at the spawn, driving), 1 warm-up and 3
runs a side in turn, the game held to 16 or to 8 logical processors by a job object (the SDK resets the process affinity at start;
this keeps the 96 MB L3, the clocks and the IPC of the 9800X3D, so it imitates the number of threads only). Frame time is the mean of
the 3 runs' medians, per swap is the thread's CPU:

| | Timer thread | Process | Frame median, base to fix | p99, base to fix |
|---|---|---|---|---|
| 16, free roam | 6.88 to 0.06 ms | 23.7 to 16.9 ms (-29%) | 6.874 to 6.883 ms (+0.1%) | 7.91 to 7.89 |
| 16, standing | 6.84 to 0.03 | 23.6 to 16.6 (-30%) | 6.829 to 6.885 (+0.8%) | 7.91 to 7.96 |
| 16, driving | 6.74 to 0.04 | 25.3 to 19.6 (-23%) | 6.854 to 6.965 (+1.6%; 3 runs each, not overlapping) | 7.93 to 8.07 |
| 8, free roam | 6.81 to 0.04 | 24.3 to 17.1 (-30%) | 6.797 to 6.868 (+1.0%) | 7.98 to 8.04 |
| 8, standing | 6.82 to 0.03 | 24.4 to 17.3 (-29%) | 6.811 to 6.882 (+1.0%) | 8.03 to 8.02 |
| 8, driving | 6.69 to 0.03 | 26.3 to 20.4 (-22%) | 6.834 to 7.001 (+2.4%) | 8.22 to 8.29 |

The saving in CPU is real and large (a whole core), and the frame is not shorter: where most is going on (driving, the job threads
busy) it is 1.6 to 2.4% longer, with the main thread's CPU +3% and +6% and the job threads' +23% and +7% there. The wait is 0.33 ms
late on average, so the guest's tick count is up to 1.3 ms old instead of 1 ms; `timer_queue_hr_spin_us` keeps the thread awake
for the last part of the wait to find out whether that lateness is the cause (it is not: the next section).

## The three options measured together (2026-10-27)

One build with the three options as cvars (`timer_queue_hr_wait`, with `timer_queue_hr_spin_us`; `guest_atomics_lock_free`;
`mcla_poll_wait`), D3D12, native, uncapped, the city route, 16 and then 8 logical processors (job object), the race at 16;
1 warm-up per group (the first variant's: the shader and file caches are warm after it) and 3 runs a side, the variants
alternating and rotated; the display woken before and checked after each run (on at both ends of all of them). The
reference ("off") is the same build with `timer_queue_hr_wait`, `guest_atomics_lock_free` and `mcla_poll_wait` off; a
separate base (the SDK as v0.1.3 ships it) agreed with it within the noise (frame -0.2 to +0.3%, p99 +0.6 to +1.3%, CPU
+-1%, except the GPU command thread +3%). Means of the 3 runs; per swap = per presented frame. The keep rule: on every
segment the frame median at most 1.0% and the p99 at most 3.0% longer than the reference (the runs' own spread is
+-0.3% in the frame median).

| 16 processors | Frame median, free roam / standing / driving | Change | p99 change | Process CPU per swap |
|---|---|---|---|---|
| reference | 6.882 / 6.817 / 6.877 ms | | | 23.67 / 23.51 / 26.11 ms |
| high-resolution timer wait | 6.899 / 6.916 / 6.966 | +0.2 / +1.4 / +1.3% | -0.9 / -0.8 / +0.4% | -30 / -28 / -28% |
| + 400 us spin tail | 6.901 / 6.906 / 6.995 | +0.3 / +1.3 / +1.7% | +1.2 / +2.2 / +2.3% | -25 / -23 / -20% |
| lock-free guest atomics | 6.847 / 6.797 / 6.827 | -0.5 / -0.3 / -0.7% | -2.0 / -0.9 / -1.1% | -2 / -1 / -3% |
| main thread's ring wait on a timer | 6.793 / 6.930 / 6.929 | -1.3 / +1.7 / +0.8% | -0.2 / -0.3 / +1.5% | -19 / -18 / -12% |
| timer wait + tail + atomics | 6.944 / 6.980 / 7.036 | +0.9 / +2.4 / +2.3% | +2.5 / +0.6 / +2.4% | -26 / -25 / -24% |

| 8 processors | Frame median | Change | p99 change | Process CPU per swap |
|---|---|---|---|---|
| reference | 6.856 / 6.839 / 6.864 ms | | | 24.09 / 24.26 / 26.13 ms |
| timer wait + 400 us tail | 6.921 / 6.942 / 6.997 | +0.9 / +1.5 / +1.9% | +2.2 / +2.3 / +0.9% | -26 / -25 / -18% |
| lock-free guest atomics | 6.853 / 6.815 / 6.849 | -0.1 / -0.4 / -0.2% | -0.5 / -0.4 / +0.1% | 0 / -1 / +3% |
| main thread's ring wait on a timer | 6.694 / 6.751 / 6.869 | -2.4 / -1.3 / +0.1% | -0.1 / -1.1 / -1.1% | -17 / -18 / -15% |
| timer wait + tail + atomics | 6.954 / 6.911 / 7.041 | +1.4 / +1.0 / +2.6% | +2.1 / +1.0 / +0.3% | -27 / -27 / -20% |

The race (16 processors, 3 runs a side): lock-free atomics, frame 6.855 to 6.840 ms (-0.2%), p99 8.59 to 8.49 (-1.2%), render thread 5.65
to 5.37 ms (-5%), process 36.19 to 35.85 (-1%).

- **Timer queue: the high-resolution wait fails the rule** with and without the spin tail, at both processor counts (+1.3 to
  +1.9% in the standing and driving segments). The tail removed the lateness (counters in the timer thread: wake-ups 306 us late on
  average without it, 6,898 of 59,633 more than 500 us late; 38 us with it, 274 of 54,774) and the frame stayed longer, so the
  late wake-up is not the cause; what is, is not known. `timer_queue_hr_wait` therefore defaults to off (v0.1.3's behaviour,
  one core spinning in the llvm-mingw builds) at 16 and 8 logical processors; the option and the tail stay for a machine that needs
  the core. Since 2026-10-27 the game turns it on by itself with 5 or fewer logical processors, where it is a large win (section
  "The timer queue and the frame-pacing thread by processor count" below).
- **Lock-free atomics: kept on.** It passes everywhere, the render thread uses 3 to 12% less CPU, the frame is 0.1 to 0.7% shorter
  (the race 0.2%). The shipped combination (this on, the timer wait off, the poll wait off) against the v0.1.3 SDK, 5 runs a
  side, is in the next section.
- **The main thread's wait for a ring slot on a timer** (`mcla_poll_wait`, `src/poll_wait.cpp`): the main thread's CPU falls by
  57 to 66% (6.40 to 2.24 ms per swap in free roam) and the process's by 12 to 19%, but the frame time was not settled with 3
  runs a side: it failed the rule once at 16 processors (+1.7% standing, +0.8% driving, -1.3% free roam) and passed at 8
  (-2.4 / -1.3 / +0.1%). The next section measures it again, 5 runs a side.

### The wait for a ring slot

Who frees a slot (`mcla_poll_find_writer`: the page of the ring is made read-only, each write is counted by the guest function it
comes from and let through; one 40 s run of the free-roam route, the game slowed a little by the trace): the word at +4 of the ring
(`0x82836818`, the one the main thread waits to change) is stored only by `sub_82160088` (5,782 stores in 40 s, 145 a second), which
`sub_82144888` calls once a round; that function is the loop of a guest thread that sleeps between rounds (a `Sleep` whose argument is worked out from the time the
round took, 72% of that thread's samples). The word at +0 (`0x82836814`) is stored by `sub_82161210`
(5,712 stores, 143 a second), the main thread's own frame function, at its end: it is the main thread's write position, not a
word it waits for. So there is no missing writer; the hook after the store in `sub_82160088` is the only place where a slot is
freed, and it was there from the start.

How the main thread's waits ended (`mcla_poll_wait` on, `mcla_poll_stats`, 40 s of the free-roam route, 16 processors): 28,950
waits, about 5 a frame; 1,770 ended in the first 40 us spin on a change of the word; 27,179 slept, 2,471 woken by the release and
24,708 by the timer, and then 1,532 ended in the tail spin on a change of the word and 23,176 at the loop's own deadline of 1,000 us.
So 20% of the waits end because a slot was freed and 80% because the game's loop gives up after a millisecond (it then sets its
flag byte and goes on). A round of the freeing thread comes once a frame, so most of a frame's waits cannot end on it. What
the poll wait saves is the CPU of waiting: a sleep on a timer for most of the millisecond instead of `Sleep(0)` and two clock
reads a turn.

### The frame time does not follow the CPU time

In every variant and segment of this section the frame median stays between 6.69 and 7.04 ms (mostly 6.8 to 6.9) while the process's
CPU per swap goes from 16.6 to 26.8 ms and the render thread's, in the city, from 3.5 to 4.1: a third of the process's CPU can go (timer queue, poll
wait) and the frame time moves by +-2%, at 16 and at 8 logical processors alike, and the race (a render thread at 82% of a core) has
the same 6.85 ms. Something other than any one thread's CPU time sets the frame time in these routes. The thread that frees the ring
slots comes round 143 to 145 times a second, once per frame, and ends each round with a `Sleep` whose argument is the float at
`0x820036EC`, less the time its round took, in whole milliseconds (only while the byte at `0x827D1998` is set). `--mcla_poll_stats`
logs that thread's rounds, their spacing, the distribution of its `Sleep` arguments and the float, with or without the poll
wait. In a 50 s run of the city route (16 processors, the defaults): 7,282 rounds, on average 6.870 ms apart (9.39 ms at most),
`Sleep` arguments of 3 ms (11 times), 4 ms (6,310) and 5 ms (961), the float 7.0 and the byte 1. With the poll wait on: 7,235
rounds, 6.912 ms apart, 4 ms (3,868) and 5 ms (3,339). So a round is 2 to 3 ms of work and waiting and a `Sleep` for the rest of
7 ms, and the frame rate in these routes equals that thread's round rate. In the first profile (free roam, 15 s) the thread
spends 73.7% of its samples in that `Sleep` and 23.5% in a second wait (`sub_82135F80`, which calls `sub_82136E10`, then the
kernel call `XMsgInProcessCall(250, 0x70009, ...)` in `sub_8240ECD0`: about 1.6 ms of a round in a host wait), 2.1% in the 24
per-frame callbacks of `sub_8214D050` and 0.5% in `sub_82160088`, the function that frees the slot; its CPU is 0.25 ms a swap. It
makes it look like the game's own limit of one round per 7 ms (143 a second); whether it sets the frame rate when nothing else
does is what a cap on that `Sleep` measures: `mcla_pump_sleep_max_ms` (`src/poll_wait.cpp`; -1, the default, leaves the game's own
`Sleep`; 0 or more caps its argument at that many milliseconds, 0 being a `Sleep(0)`; read every round, so it can be changed while
running). It exists for that experiment and is not a setting for players: the audio work (`launcher/dev/audio-2026-10-27.md`, batch E) found it
unsafe on 4 logical processors, where a cap of 2 or 1 ms gave 859 and 504 steady audio underruns (none at 16, where the frame rate rose from 145 to 202 and 239 FPS).

## The shipped defaults against v0.1.3, and the ring wait again (2026-10-27)

One build with the defaults as shipped (the timer queue's high-resolution wait off, lock-free guest atomics on, the main thread's
ring wait off; no option flags) against the SDK as v0.1.3 ships it, and the same build with `--mcla_poll_wait=true`; D3D12, native,
uncapped, the city route at 16 and at 8 logical processors (job object) and the race at 16, 1 warm-up per group, 5 runs a side in
the city and 3 in the race, the variants rotated, the display woken before and checked after every run (on at both ends of all of
them). Per swap, means of the runs; the rule as above (frame median at most 1.0% and p99 at most 3.0% longer on every segment).

| | Frame median: free roam / standing / driving | p99 | Render thread | Process |
|---|---|---|---|---|
| 16, v0.1.3 | 6.882 / 6.804 / 6.850 ms | 7.96 / 7.92 / 8.08 | 3.85 / 3.87 / 3.92 | 23.87 / 23.56 / 26.12 |
| 16, shipped defaults | 6.853 / 6.789 / 6.843 (-0.4 / -0.2 / -0.1%) | 7.93 / 7.89 / 8.02 (-0.3 / -0.4 / -0.7%) | 3.53 / 3.53 / 3.79 (-8 / -9 / -3%) | 23.14 / 23.01 / 26.08 (-3 / -2 / 0%) |
| 8, v0.1.3 | 6.835 / 6.837 / 6.851 | 7.99 / 8.05 / 8.20 | 3.82 / 3.92 / 4.00 | 24.03 / 24.19 / 26.49 |
| 8, shipped defaults | 6.869 / 6.803 / 6.847 (+0.5 / -0.5 / -0.1%) | 7.97 / 8.00 / 8.36 (-0.3 / -0.6 / +1.9%) | 3.55 / 3.65 / 4.05 (-7 / -7 / +1%) | 23.95 / 23.78 / 27.53 (0 / -2 / +4%) |
| race at 16, v0.1.3 | 6.895 | 8.52 | 5.87 | 36.44 |
| race at 16, shipped defaults | 6.854 (-0.6%) | 8.51 (-0.1%) | 5.23 (-11%) | 35.98 (-1%) |

The shipped combination passes the rule on every segment, and the frame time is within +-0.6% of v0.1.3's everywhere (the runs'
own spread is +-0.3%). The saving is the render thread's CPU: 3 to 9% in the city (one segment of the 8-processor runs 1% more)
and 11% in the race, where that thread is the busiest, 0.1 to 0.6 ms a swap; the process's total falls 2 to 3% at 16 processors
and not at 8 (the driving segment at 8 has the GPU command thread's CPU 14% and the other threads' 10% higher with the frame
unchanged, which the 16-processor runs of the same builds do not show).

The main thread's ring wait (`mcla_poll_wait`), against the shipped defaults:

| | Frame median: free roam / standing / driving | p99 | Main thread | Process |
|---|---|---|---|---|
| 16, shipped defaults | 6.853 / 6.789 / 6.843 ms | 7.93 / 7.89 / 8.02 | 6.44 / 6.38 / 6.40 | 23.14 / 23.01 / 26.08 |
| 16, ring wait | 6.784 / 6.832 / 6.916 (-1.0 / +0.6 / +1.1%) | +0.8 / +1.2 / +1.6% | 2.32 / 2.14 / 2.44 (-64%) | 19.29 / 18.89 / 22.13 (-17%) |
| 8, shipped defaults | 6.869 / 6.803 / 6.847 | 7.97 / 8.00 / 8.36 | 6.35 / 6.32 / 6.17 | 23.95 / 23.78 / 27.53 |
| 8, ring wait | 6.687 / 6.829 / 6.895 (-2.6 / +0.4 / +0.7%) | +0.7 / -0.3 / -1.3% | 2.43 / 2.41 / 2.51 (-61%) | 19.49 / 19.60 / 22.10 (-19%) |

The main thread's CPU falls by 61 to 64% and the process's by 17 to 19%, and the frame time does not follow: free roam is
1.0 to 2.6% shorter, standing and driving 0.4 to 1.1% longer. At 16 processors it fails the rule in the driving segment, by 0.1
point (+1.1%; p99 +1.6%), at 8 it passes. The runs of the ring wait fall into two groups in free roam (6.67 to 6.76 and 6.89 to
6.93 ms at 16), the width of one millisecond of the freeing thread's `Sleep`. So `mcla_poll_wait` stays off by default: it is an
option for a CPU with few cores or little headroom, where 17 to 19% of the process's CPU matters more than a frame time that is
1% longer in the busiest segment.

Counted in the game (16 processors, the city route, `--mcla_poll_stats`, the ring wait on): 29,851 calls of the wait in 50 s
(about 5 for each round of the freeing thread), 3,693 ended in the first spin, 26,158 slept (780 woken by the release, 25,378 by
the timer), 1,185 ended in the tail spin on a change of the word and 24,193 at the loop's deadline; the word was set 7,235 times:
the same shape as the 40 s run above, 19% of the waits end on a freed slot. With the writer trace (the ring wait off, 40 s) the
word at +4 was stored 5,821 times by `sub_82160088` and the word at +0 5,768 times by `sub_82161210`, and no store to the flag
byte at `0x82836810` was counted at all: the game's own loop never gave up on a slot in that run, while with the ring wait on
four in five waits run to the deadline. How those two agree is not explained yet.

## Wake on demand: the frame-pacing thread comes round when the main thread waits for it (2026-10-27)

The finding above (the thread that frees the command ring's slots sleeps out 7 ms per round, and its round rate is the frame rate) gives
the uncapped game a ceiling of about 143 to 145 FPS on a CPU whose frame is faster than 7 ms. Capping that `Sleep`
(`mcla_pump_sleep_max_ms`) lifts the ceiling (202 and 239 FPS at 16 logical processors) but runs the thread all the time, costs CPU
at 30 and 60 FPS with no gain, and starved the audio worker at 4 logical processors. `mcla_pump_wake_on_demand` (`src/poll_wait.cpp`,
`config/poll_wait.toml`; on by itself with 6 or more logical processors, see the A/B below) does it only when it is needed:

- the hook `mcla_poll_tick` (`0x821449C8`, after the load of the `Sleep` argument in `sub_82144888`) now returns a bool, and
  `jump_address_on_true = 0x821449D0` skips the game's `bl sub_821C91B8`; nothing reads the `Sleep`'s result. With the option on the
  hook does the wait itself: `WaitForSingleObject` on a manual-reset event with the `Sleep`'s argument (scaled like `XThread::Delay`
  scales it), after clearing the event; the thread marks itself as waiting around it. A `Sleep(0)` is left to the game.
- the hook `mcla_poll_wait` (`0x82148AB0`, in the main thread's wait for a free slot, `sub_82148A10`) is called every turn of that loop whether or
  not `mcla_poll_wait` is on; it now sets the event when the thread is waiting and the ring is the first one (the one `sub_82160088` frees; the byte at
  `+199` of the context picks it). So the round comes at once when the main thread has run out of work and waits for it, and
  otherwise the thread keeps its 7 ms rhythm: nothing sets the event while a frame takes the main thread longer than the rhythm, or while a
  frame-rate cap holds the main thread back, so a CPU that cannot exceed 145 FPS and the 30, 60 and 120 FPS caps see no change.
- the audio watchdog: the SDL driver's steady underrun counter (silent frames after the game's first second of audio) is now counted
  always (`rex::audio::dump::SteadyUnderruns()`, `patches/rexglue-xma-decoder-fixes.patch`), and the thread checks it once a round; a rise less than
  two seconds after a round that came early turns the option off for the rest of the session with one warning in the log (an underrun with no
  early round in the last two seconds is not its doing and is ignored). `--mcla_pump_wake_test_trip_s=N` trips it N seconds after the first early
  round, to test the way back (development).
- `mcla_pump_sleep_max_ms` stays as a development option; with it set (0 or more) the wake does nothing. The dev pad's `state` reply has
  `pump_early=N` next to `pump_rounds=N`; `--mcla_poll_stats` logs the waits, the early ones and how often the ring wait saw the thread waiting.

Checked so far (single runs under the shared lock, so they say how it behaves, not how much): the city route of
`launcher/dev/scripts/wake-ab.pad` at 16 logical processors with the option on ran at 239 to 240 FPS uncapped (frame median 4.15 to 4.17 ms,
p99 4.8 ms; the thread's rounds equal the frames, every one of them early) and held the caps (120.0, 60.0 and 30.0 swaps a second, no early round
under a cap, the game clock 1.002 to 1.003 s a second); the ring wait only ever saw ring 0 with the thread waiting; with the watchdog tripped by
`--mcla_pump_wake_test_trip_s=20` the frame rate fell back to 146 FPS and the log said so.

### The A/B

D3D12, native, `launcher/dev/scripts/wake-ab.pad` (the title screen, then the city: free roam for 12 s and driving for 14 s, uncapped, then
caps of 120, 60 and 30 FPS set with `set mcla_fps`, 12 s measured each after 3 s of settling), real-time steps, the game limited to 16, 8, 6 and 4
logical processors (the harness sets the process's affinity mask to 0xFFFF, 0xFF, 0x3F and 0xF), the same build with
`--mcla_pump_wake_on_demand=false` ("off", the game's own Sleep) and `=true` ("on"), 1 warm-up and 3 runs a side in turn (rotated), every run with
`--audio_stats` (every 2 s) and `--mcla_poll_stats`, under the exclusive lock with the display woken before and checked after every run (on at both
ends of all of them). Means of the 3 runs; "to" is off to on, in brackets the change; the process's CPU is the time of all its threads per second.
The checks: RULE (the frame median at most +1.0% and p99 at most +3.0% longer on every segment, as for the other options), GAIN (free roam and
driving at least 3% shorter where the base reached 142 FPS, i.e. where the Sleep is what holds it), AUDIO (no steady underrun in any run of the
variant and the watchdog never tripped), CAPS (swaps a second within 1% of 120, 60 and 30) and CLOCK (the game clock within 0.01 s/s of 1.000).

| Logical processors | Segment | Frame median, ms | p99, ms | Swaps a second | Process CPU, ms a second | Pump rounds a second | Game clock |
|---|---|---|---|---|---|---|---|
| 16 | free roam | 6.819 to 4.176 (-38.8%) | 7.94 to 4.84 (-39.1%) | 146.0 to 239.4 (+64.0%) | 3356 to 3928 (+17.1%) | 146 to 239 | 1.003 / 1.003 |
|  | driving | 6.822 to 4.151 (-39.2%) | 7.87 to 4.80 (-38.9%) | 145.7 to 240.1 (+64.7%) | 3292 to 3922 (+19.1%) | 146 to 240 | 1.003 / 1.003 |
|  | cap 120 | 8.341 to 8.338 (-0.0%) | 8.90 to 8.92 (+0.3%) | 120.0 to 120.0 (-0.0%) | 2511 to 2545 (+1.3%) | 145 to 153 | 1.002 / 1.002 |
|  | cap 60 | 16.665 to 16.664 (-0.0%) | 17.34 to 17.22 (-0.7%) | 60.0 to 60.0 (+0.0%) | 2095 to 2061 (-1.6%) | 145 to 153 | 1.003 / 1.003 |
|  | cap 30 | 33.331 to 33.324 (-0.0%) | 33.94 to 33.96 (+0.0%) | 30.0 to 30.0 (-0.0%) | 1851 to 1787 (-3.5%) | 145 to 153 | 1.002 / 1.002 |
| 8 | free roam | 6.836 to 4.278 (-37.4%) | 8.12 to 5.69 (-29.8%) | 145.6 to 230.1 (+58.1%) | 3442 to 4110 (+19.4%) | 146 to 230 | 1.002 / 1.003 |
|  | driving | 6.828 to 4.183 (-38.7%) | 8.10 to 5.25 (-35.1%) | 145.8 to 238.2 (+63.4%) | 3417 to 4220 (+23.5%) | 146 to 238 | 1.003 / 1.003 |
|  | cap 120 | 8.336 to 8.327 (-0.1%) | 9.08 to 9.14 (+0.7%) | 120.0 to 120.0 (-0.0%) | 2657 to 2563 (-3.5%) | 145 to 154 | 1.002 / 1.003 |
|  | cap 60 | 16.662 to 16.665 (+0.0%) | 17.35 to 17.38 (+0.2%) | 60.0 to 60.0 (+0.0%) | 2092 to 2122 (+1.4%) | 145 to 153 | 1.002 / 1.002 |
|  | cap 30 | 33.338 to 33.326 (-0.0%) | 34.03 to 34.09 (+0.2%) | 30.0 to 30.0 (+0.0%) | 1744 to 1781 (+2.1%) | 145 to 153 | 1.002 / 1.002 |
| 6 | free roam | 6.863 to 4.494 (-34.5%) | 8.12 to 6.21 (-23.5%) | 145.6 to 217.4 (+49.3%) | 3590 to 4093 (+14.0%) | 146 to 218 | 1.003 / 1.003 |
|  | driving | 6.838 to 4.247 (-37.9%) | 8.31 to 6.04 (-27.3%) | 145.6 to 232.4 (+59.6%) | 3464 to 4263 (+23.1%) | 146 to 233 | 1.002 / 1.003 |
|  | cap 120 | 8.341 to 8.329 (-0.2%) | 9.16 to 9.14 (-0.2%) | 120.0 to 120.0 (-0.0%) | 2593 to 2606 (+0.5%) | 145 to 153 | 1.002 / 1.002 |
|  | cap 60 | 16.661 to 16.660 (-0.0%) | 17.39 to 17.45 (+0.4%) | 60.0 to 60.0 (-0.0%) | 2102 to 2129 (+1.3%) | 145 to 154 | 1.002 / 1.002 |
|  | cap 30 | 33.332 to 33.331 (-0.0%) | 34.16 to 34.25 (+0.3%) | 30.0 to 30.0 (-0.0%) | 1865 to 1796 (-3.7%) | 145 to 154 | 1.002 / 1.002 |
| 4 | free roam | 7.013 to 6.295 (-10.2%) | 29.64 to 25.30 (-14.7%) | 118.6 to 137.5 (+15.9%) | 3764 to 3770 (+0.2%) | 119 to 140 | 1.003 / 1.003 |
|  | driving | 6.872 to 6.340 (-7.8%) | 29.91 to 29.58 (-1.1%) | 116.1 to 128.9 (+11.1%) | 3760 to 3796 (+0.9%) | 117 to 133 | 1.003 / 1.003 |
|  | cap 120 | 8.303 to 8.296 (-0.1%) | 22.31 to 26.61 (+19.3%) | 117.0 to 116.1 (-0.8%) | 2797 to 2807 (+0.4%) | 139 to 144 | 1.003 / 1.003 |
|  | cap 60 | 16.629 to 16.628 (-0.0%) | 41.37 to 41.35 (-0.1%) | 59.0 to 59.0 (-0.1%) | 2365 to 2345 (-0.8%) | 137 to 143 | 1.003 / 1.003 |
|  | cap 30 | 33.317 to 33.329 (+0.0%) | 64.26 to 64.55 (+0.4%) | 30.0 to 30.0 (-0.1%) | 2108 to 2025 (-3.9%) | 133 to 139 | 1.003 / 1.002 |

| Logical processors | Steady underruns, off / on (3 runs each) | Late wake-ups of the audio worker (8 ms or more) a run, off / on |
|---|---|---|
| 16 | 0 0 0 / 0 0 0 | 0 0 0 / 0 0 0 |
| 8 | 0 0 0 / 0 0 0 | 0 0 0 / 0 0 0 |
| 6 | 0 0 0 / 0 0 0 | 0 0 0 / 3 0 3 |
| 4 | 0 0 0 / 0 2 0 | 329 303 329 / 252 330 249 |

(Late wake-ups: the audio worker's wake-ups 8 ms or more late, per run; "steady underruns" are the silent frames after the game's first second
of audio.) The 6-logical "on" runs had 3 late wake-ups in two of the three runs, none in the third.

- **16, 8 and 6 logical processors: every check passes.** Uncapped, the city goes from 146 to 217-240 swaps a second (+49 to +65%; free roam
  6.8 ms a frame to 4.2-4.5, driving 6.8 to 4.2) and the p99 frame is 23 to 39% shorter. In the uncapped segments every round of the thread is early (the early
  wakes a second equal its rounds, which equal the swaps) and under a cap none is: the caps hold (120.0, 60.0, 30.0 swaps a second, the frame within 0.2% and the p99 within
  -0.7 to +0.7%). The game clock stays at 1.002 to 1.003 s a second in every segment (real-time steps). No steady underrun in the 18 runs
  and the watchdog never tripped. The process's CPU a second is +14 to +24% uncapped (it makes 49 to 65% more frames, so per frame it is 23 to 29%
  less) and -3.7 to +2.1% under the caps. Under the caps the thread's rounds run 5 to 6% faster than with the game's own Sleep (153 against 145 a second): the
  event's timeout ends about 0.33 ms sooner than the Sleep's; no CPU cost showed (process CPU within the above).
- **4 logical processors: it fails.** Uncapped, only +16% and +11% (118.6 to 137.5 and 116.1 to 128.9 swaps a second; the frame median -10.2% and
  -7.8%): the CPU is the limit there, not the Sleep. One of the three "on" runs (r2) had 2 steady underruns, 2 silent frames, while the thread was
  coming round early: the watchdog tripped and the log says so ("pump wake on demand: the audio output ran dry (2 silent frames) while the thread
  was coming round early; the frame-pacing thread goes back to the game's own Sleep for the rest of this session"); the thread went back to the game's own Sleep for the rest of that run.
  The p99 at the 120 cap is 19.3% longer (22.3 to 26.6 ms) and the cap does not hold either way (117.0 and 116.1 swaps a second; the 60 cap 59.0 in both). The audio
  worker's late wake-ups (8 ms or more) are 252 to 330 a run with it and 303 to 329 without. So RULE (cap 120), AUDIO and CAPS fail, and the option is not used there.
  With the missing core of the timer thread (next section) the 4-logical machine is helped by the other option instead.
- **Speed check** (`launcher/dev/scripts/cpup-speed.pad`, 16 logical processors, real-time steps, one run a side, the car driven for 14 s of wall time): the engine
  frames equal the swaps, the game clock runs at 1.002 s a second without and 1.003 with the option, and the car covers the same ground (a path of 31.3 and
  31.0 units, 2.24 and 2.21 a second along it; the engine step falls from a mean of 6.86 to 4.17 ms), so the game is not faster or slower for
  playing at 240 instead of 146 FPS. In that pad's 8 s windows under the caps the swaps were 120.2, 60.2 and 30.5 a second without and 122.6, 60.7 and 30.8
  with the option; the windows start while the cap takes hold (the 5.2 early wakes a second at the 120 cap are the first uncapped moments), and the A/B above,
  which measures 12 s after 3 s of settling, has 120.0, 60.0 and 30.0.
- **The default** is on at 6 or more logical processors and off at 5 or fewer (section "The timer queue and the frame-pacing thread by processor count").
  The count is the process's affinity mask, which is how the harness limits the game, so the A/B above is what a player with that many processors gets.

## The timer queue and the frame-pacing thread by processor count (2026-10-27)

The two options above each win on one side of a processor count and lose on the other, so the game chooses by the number of logical processors the
process may run on (the popcount of the process's affinity mask, which includes a job object's limit; when the mask is the whole processor group, all
groups' processors, for a machine with more than 64). `MclaApp::ApplyPcDefaults` (`src/mcla_app.cpp`, Windows) sets, as the options' defaults
(`rex::cvar::SetDefaultValue`: a value from the command line, an environment variable or `mcla.toml` wins, and nothing is saved):

| Logical processors | `mcla_pump_wake_on_demand` | `timer_queue_hr_wait` |
|---|---|---|
| 6 or more | on | off |
| 5 or fewer | off | on |

and logs one line: `CPU: 4 logical processors: timer queue high-resolution wait on, pump wake on demand off (automatic; 5 or fewer)`, with
`(set by the player)` after a value that was. Both cvars are read where they are used (the timer thread at every wait, the pump thread at every
round), so the default set at the start is in effect for the whole run. Linux and the PS5 keep both off.

**The timer queue at 6 and 4 logical processors.** The same harness as the wake A/B (D3D12, native, the city route with the caps, 1 warm-up and 3 runs a
side, exclusive lock, display awake), `timer_queue_hr_wait` false ("off", v0.1.3's behaviour) and true ("on"), `mcla_pump_wake_on_demand` off in both. The
last column is the timer thread's CPU in ms per swap.

| Logical processors | Segment | Frame median, ms | p99, ms | Swaps a second | Process CPU, ms a second | Timer thread, ms a swap |
|---|---|---|---|---|---|---|
| 6 | free roam | 6.867 to 6.898 (+0.4%) | 8.05 to 8.22 (+2.1%) | 145.8 to 144.2 (-1.1%) | 3561 to 2467 (-30.7%) | 6.79 to 0.03 |
|  | driving | 6.828 to 6.943 (+1.7%) | 8.27 to 8.22 (-0.7%) | 145.9 to 143.5 (-1.7%) | 3478 to 2370 (-31.9%) | 6.80 to 0.04 |
|  | cap 120 | 8.336 to 8.336 (-0.0%) | 9.11 to 8.94 (-1.9%) | 120.0 to 120.0 (+0.0%) | 2578 to 1544 (-40.1%) | 8.32 to 0.03 |
|  | cap 60 | 16.663 to 16.668 (+0.0%) | 17.27 to 17.26 (-0.0%) | 60.0 to 60.0 (-0.0%) | 2096 to 1119 (-46.6%) | 16.63 to 0.06 |
|  | cap 30 | 33.324 to 33.322 (-0.0%) | 34.29 to 33.94 (-1.0%) | 30.0 to 30.0 (-0.0%) | 1749 to 867 (-50.4%) | 30.71 to 0.19 |
| 4 | free roam | 7.011 to 7.053 (+0.6%) | 26.77 to 9.82 (-63.3%) | 123.1 to 144.0 (+17.0%) | 3798 to 2939 (-22.6%) | 7.07 to 0.04 |
|  | driving | 6.885 to 6.835 (-0.7%) | 30.40 to 11.91 (-60.8%) | 117.5 to 142.7 (+21.4%) | 3739 to 2771 (-25.9%) | 7.50 to 0.03 |
|  | cap 120 | 8.295 to 8.338 (+0.5%) | 23.77 to 9.13 (-61.6%) | 117.0 to 120.0 (+2.6%) | 2830 to 1651 (-41.7%) | 8.26 to 0.05 |
|  | cap 60 | 16.648 to 16.668 (+0.1%) | 39.11 to 17.35 (-55.6%) | 59.2 to 60.0 (+1.3%) | 2340 to 1113 (-52.4%) | 16.66 to 0.06 |
|  | cap 30 | 33.328 to 33.316 (-0.0%) | 63.02 to 34.18 (-45.8%) | 30.0 to 30.0 (+0.0%) | 2058 to 862 (-58.1%) | 32.35 to 0.18 |

| Logical processors | Steady underruns, off / on (3 runs each) | Late wake-ups of the audio worker (8 ms or more) a run, off / on |
|---|---|---|
| 6 | 0 0 0 / 0 0 0 | 3 2 1 / 0 0 0 |
| 4 | 0 0 0 / 0 0 0 | 301 307 276 / 25 16 23 |

- **At 4 logical processors the high-resolution wait is a large win and passes every check.** The timer thread's CPU falls from the whole frame time per
  swap (one core) to 0.03 to 0.19 ms, and the process's by 23 to 58%. The median frame does not move (+0.6% and -0.7% uncapped) but the long frames go:
  p99 26.8 and 30.4 ms to 9.8 and 11.9 ms (-63% and -61%), the swaps a second 123.1 and 117.5 to 144.0 and 142.7 (+17.0% and +21.4%), the 120 FPS cap
  now held (117.0 to 120.0), the 60 cap 59.2 to 60.0, the p99 at the caps 46 to 62% shorter. The audio worker's wake-ups 8 ms or more late fall from
  about 300 to about 20 a run. No steady underrun either way. (The "off" runs of this and of the wake A/B agree: 116 to 123 swaps a second.)
- **At 6 it fails the rule narrowly** (RULE and GAIN): the frame +0.4% and +1.7% in free roam and driving (145.8 and 145.9 to 144.2 and 143.5 swaps a second),
  p99 +2.1% and -0.7%, for 31 to 50% less CPU (-31% and -32% uncapped, -40 to -50% at the caps) and the late wake-ups 3 2 1 to 0 0 0. Earlier, at 16 and 8, it made the frame 1 to
  2.4% longer (above), so it stays off from 6 up.
- **The line at 6**: wake on demand passes at 6 (+49 to +60% FPS, no audio problem) and fails at 4; the high-resolution wait passes at 4 and fails at 6. Neither
  was measured at 5, which follows the 4 (5 or fewer). Nothing turns both on at once: they were not measured together.
- **The automatic default, checked in the game** (the exe built from this branch; no flag for either option; `wake-ab.pad`, D3D12; the `CPU:` line is
  in the game's log). 16 logical processors, one run under the shared lock: `CPU: 16 logical processors: timer queue high-resolution wait off, pump wake on
  demand on (automatic; 6 or more)`; free roam and driving 237.9 and 240.0 swaps a second (the A/B's "on": 239.4 and 240.1), the caps 120.0, 60.0 and 30.0, the
  thread's wakes early in the uncapped segments only. With `--mcla_pump_wake_on_demand=false` the line says `pump wake on demand off (set by the player)` and
  the game runs at 146.2 and 146.0 (the shipped Sleep). 4 logical processors, exclusive lock, 1 warm-up and 2 runs a side, the automatic default ("on") against
  `--timer_queue_hr_wait=false` ("off"): `CPU: 4 logical processors: timer queue high-resolution wait on, pump wake on demand off (automatic; 5 or fewer)`,
  and with the flag `timer queue high-resolution wait off (set by the player)`. The default is the measured "on": uncapped 120.9 and 117.1 to 144.3 and
  142.4 swaps a second (+19.4% and +21.6%), p99 27.0 and 31.5 ms to 9.5 and 12.6 ms (-65% and -60%), the 120 cap 115.5 to 120.0, the 60 cap 59.2 to 60.0,
  the process's CPU -23 to -58%, the audio worker's late wake-ups 340 and 288 a run to 19 and 21; one "off" run had a steady underrun (1 silent frame), no "on" run. 6 logical processors, the boundary: `CPU: 6 logical processors: timer queue
  high-resolution wait off, pump wake on demand on (automatic; 6 or more)`.

**Not verified**: the numbers are one CPU (a Ryzen 7 9800X3D, 8 cores, 16 threads) limited by affinity, so the 4 and 6 logical-processor runs have its fast cores and cache,
not a slower or a 4-core CPU's; processor counts other than 16, 8, 6 and 4; both options together; the PS5 and Linux (neither built nor run with the options).

## Upper bounds for two codegen costs (2026-10-27)

Timing-only builds, not exact: `nocsr` makes the SSE flush-mode switch (`ldmxcsr`) do nothing, `nolock` makes the global lock around the
guest's atomics do nothing. D3D12, native, uncapped, 16 logical processors, 3 runs a side; CPU of the render guest thread per swap
(free roam / standing / driving): base 3.71 / 3.84 / 3.97 ms; `nocsr` 3.49 / 3.55 / 3.75 (-6 / -7 / -5%); `nolock` 3.58 / 3.57 / 3.65
(-3 / -7 / -8%); the process 23.8 / 23.5 / 26.1 ms, `nocsr` -4 / -2 / -3%, `nolock` -3 / -2 / -6%. The frame time did not change in
either (-0.5 to +0.1%). So the lock is worth removing (`rexglue-guest-atomics-lock-free.patch`). For `ldmxcsr`: the code generator
already tracks the flush mode through a function (it writes `disableFlushModeUnconditional` or `enableFlushModeUnconditional` only
where it knows the mode is the other one, and the checked `disableFlushMode` / `enableFlushMode`, a compare against the register's
shadow, where it does not), so every `ldmxcsr` that runs is a change of mode that the next instruction needs: the scalar floating-point
unit and the vector unit are one MXCSR here and have different denormal rules on the console. Removing one is not exact unless the
instruction between does not care, and which do cannot be told statically (a denormal operand or result is what shows). What is left
is not removing switches but not needing them (running a scalar operation in the vector mode when neither operand nor result is a
denormal, with a check for it): a change to every floating-point builder, for at most the 5 to 7% of the render thread measured above.
Not done.

## `execute_unclipped_draw_vs_on_cpu` on and off (2026-10-27)

D3D12, native, uncapped, 3 runs a side, 16 and 8 logical processors (the fork's PC default is on): the GPU command thread's CPU per
swap with it off, against on: -6 / +2 / -7% at 16 (free roam / standing / driving) and -9 / -1 / -3% at 8, the frame time -0.3 to +0.6%
(the spread of the runs is +-0.3%): nothing to choose between them at native resolution. The setting stays as it is.

## The GPU command thread, with names (P1 windows, DLLs rebuilt with line tables and the same code)

`GPU_Commands` uses 3.2 to 3.3 ms of CPU per swap in free roam (it waits for commands 56 to 62% of the time), 4.9 ms in the
race (32% waiting; 5.8 ms and 21% on Vulkan). It is not what limits a frame at native resolution (in the race the render guest
thread is), and nothing in it is above 5% of its samples. Inclusive shares of the thread's samples, D3D12 free roam / race:

| What | Free roam | Race |
|---|---|---|
| Draws (`ExecutePacketType3Draw` -> `D3D12CommandProcessor::IssueDraw`, nested indirect buffers included) | 30% (2.1 ms) | 48% |
| `UpdateBindings` (descriptors and constants per draw; self time 3.2% in free roam) | 5.8% | 12% |
| `D3D12TextureCache::RequestTextures` | 4.3% | 8% |
| `D3D12RenderTargetCache::Update` | 4.8% | 6.4% |
| `SharedMemory::RequestRange` called from `IssueDraw` (and again from `PrimitiveProcessor::Process`, 2.5 and 2.3%) | 2.1% | 4.7% |
| ... of which `EnablePhysicalMemoryAccessCallbacks` -> `VirtualProtect` | under 1% | 1.9% |
| `PrimitiveProcessor::Process` | 3.0% | 2.7% |
| `PipelineCache::ConfigurePipeline` | 2.0% | 2.2% |
| Register writes (`ExecutePacketType0` -> `WriteRegisterRangeFromRing`) | 3.5% | 4.8% |
| The swap packet (`IssueSwap`) | 1.5% | 1.5% |

Vulkan has the same shape (`UpdateBindings` 7 to 8.5%, a third of it inside the driver). The candidates for a saving are
`UpdateBindings` and the two `RequestRange` calls per draw, each about 0.1 to 0.3 ms per swap in free roam: together perhaps
10 to 15% of this thread's CPU, with no effect on the frame time at native resolution on this PC, so they matter only for a
machine on which this thread is the limit. The full trees are `gpu-tree.txt` in the scratch tree's results.

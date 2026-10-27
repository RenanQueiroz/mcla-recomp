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
  one core spinning in the llvm-mingw builds); the option and the tail stay for a machine that needs the core.
- **Lock-free atomics: kept on.** It passes everywhere, the render thread uses 3 to 12% less CPU, the frame is 0.1 to 0.7% shorter
  (the race 0.2%). The shipped combination (this on, the timer wait off, the poll wait off) is measured against the v0.1.3 SDK
  in the next run; against the reference above it is this row.
- **The main thread's wait for a ring slot on a timer** (`mcla_poll_wait`, `src/poll_wait.cpp`): the main thread's CPU falls by
  57 to 66% (6.40 to 2.24 ms per swap in free roam) and the process's by 12 to 19%, but the frame time is not settled: it fails
  the rule once at 16 processors (+1.7% standing, +0.8% driving, -1.3% free roam) and passes at 8 (-2.4 / -1.3 / +0.1%). The signs
  are mixed, so the runs' spread may be most of it; it stays off until a longer comparison clears it.

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
slots comes round 143 to 145 times a second, once per frame;
whether that thread's own `Sleep` (its argument is the float at `0x820036EC`, less the time its round took, in whole milliseconds)
sets the 6.85 ms is the next thing to find out. `--mcla_poll_stats` now also logs that thread's rounds, their spacing, the
distribution of its `Sleep` arguments and the float, with or without the poll wait.

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

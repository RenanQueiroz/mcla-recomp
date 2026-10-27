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
   waits for the render thread to release the next slot by calling guest `Sleep(0)` (`sub_82460270` ->
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

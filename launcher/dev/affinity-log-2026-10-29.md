# The "too few processor cores" warning, once per run (issue #21, 2026-10-29)

**State: the patch is built and compile-checked by CI, and the chain below ran on 2026-10-29 (Results at the end): the change is log hygiene, not a speed-up.** Linux and the PS5 build were not built or run.

## What the report showed

Fork issue #21: an i5 of the 3rd generation (4 cores, 4 threads), an RX 580, v0.3.0, 30 FPS cap, audio and music stutter while the car moves. Its log (two parts, 57 s of play):

- "Too few processor cores - scheduling will be wonky": 58,797 lines, 1,000 to 1,150 a second, 54,134 from one game thread.
- Audio: 70 to 80 underruns a second at the worst; the audio pump's rounds took 4 to 9 ms (`pump: avg_us`); on the test PC (Ryzen 7 9800X3D, 16 logical processors) a round takes 0.014 ms, and 1 to 1.5 ms with four busy processes sharing four of its logical processors.
- The 1,805 "audio underrun in the middle of the game's audio" lines are the audio stats' own (they are logged only with `--audio_stats`, which that log had on), one per run of silence.

## Where the line comes from

`XThread::SetActiveCpu` (`sdk/src/system/xthread.cpp`) is called by the game's `KeSetAffinityThread` (`xboxkrnl_threading.cpp`). With `std::thread::hardware_concurrency()` below 6 it logged the warning instead of setting the affinity, on every call. With 6 or more it sets the affinity only if `ignore_thread_affinities` is off, and that is on by default, so no PC applies the game's affinities by default: the warning only ever described a difference that had no effect.
A warning-level line is written to the rotating log file and flushed (the log flushes at info), and sent to the debugger channel, under the sinks' mutex, which every thread that logs takes.

## The change

`patches/rexglue-affinity-warning-once.patch` (SDK, `src/system/xthread.cpp`):
- `affinity_warning_once` (default true): the line is logged once per process. Its text now says what it means. `--affinity_warning_once=false` is the old behaviour (every call).
- `debug_affinity_logical_processors` (default 0): only `SetActiveCpu` reads it. N makes it behave as if the PC had N logical processors. The test PC has 16, and the earlier 4-core tests limited the game with an affinity mask (a job object), which does not change `hardware_concurrency()`: the warning path never ran in them.
- Everything else that counts processors is untouched (`mcla_small_cpu_defaults` reads the mask and the topology itself; the pipeline creation threads use `logical_processor_count()`).
- Other per-call logs on that path: none. Everything else in `xthread.cpp` and `xboxkrnl_threading.cpp` at warning or above reports a failure.

## The chain (as planned; what ran differently is under Results)

One exclusive hold of at most 40 minutes (scripts kept outside the repository: chain `affinitylog`): the branch `fix/affinity-warn-once` built once with its own SDK, then runs on that one build, scratch save and shader storage only, Direct3D 12 on the RTX 5080, native resolution, `--mcla_fps=30`, `--audio_stats=true --audio_stats_interval=1`, `--debug_affinity_logical_processors=4`:

- group P: affinity mask 0x55 (4 logical processors on 4 cores, one logical processor of each core: the shape of the reporter's PC), set with a job object on the game;
- group S: the same plus 4 busy processes in the game's job object, normal priority (an emulation of a slow CPU, as in the `audio21` chain, whose stress runs took the game from 0.47 to 0.80 cores of a mask of 4 and made 22% of the frames slow);
- A = `--affinity_warning_once=false` (a warning on every call), B = the default (once); 4 runs of each in each group, interleaved (P: A B, B A, A B, B A; S: B A, A B, B A, A B), a warm-up run first (it fills the shader storage and proves the route).

Each run: boot to the city, stand 6 s, a timed start out of the car park (the one of `scripts/drive-spawn.pad`, in game time), then 30 s of driving at the cap of 30, the measured part. The car is driven by the dev pad's `follow` along the known street (a route made from a trace of a real drive; if the car stands still in the warm-up, by the inputs of the occlusion work's drive instead). The position is logged every 2 s: a run whose car stood still (two windows in a row under 4 units/s, or a mean under 5) is **not used**, and is named in the report. Also not used, and redone if time allows: a run whose options (`get`) are not what its variant says, A with fewer than 200 warnings, B with other than 1, the wrong affinity or processor count, a busy process missing at the end, a display that was off, no audio device, an unclean GPU guard.

Per run: the warning count (whole run, and per second while driving; the share of the busiest thread), the log's size, the audio stats of the driving part (the pump round's `avg_us` and `max_us`, rounds that started 8 ms or more after the previous one, rounds that took over 4 ms, the audio worker's wake-ups of 8 ms or more, silent frames and steady silent frames from the SDL callbacks), the frame times (median, p99, mean FPS), the process CPU (per-thread times once a second: cores used, ms per swap, the busiest thread names). Per group the report gives the mean and range of each side and the difference B - A with a 95% interval (Welch); with 4 runs a side the intervals are wide, and a difference inside them is said to be inside the noise.

What it cannot show: one machine (Ryzen 7 9800X3D, cores two to three times as fast as a 3rd generation i5's, RTX 5080, a lot of RAM); the busy processes are an emulation; the audio is counted, not listened to; Direct3D 12 only.

## Results

Ran on 2026-10-29, one hold. Differences from the plan above: the route was the audio work's pad `au-ab.pad` (the warm-up car moved 380 units in 24 s, 15.9 units/s, not stuck) and there were 3 blocks, so 3 runs a side in each group: 12 runs, all usable, none skipped or discarded, plus the warm-up. The build was the branch's own (cut from `main`), not the integrated tree. RTX 5080, Direct3D 12, native resolution, a 30 FPS cap, mask 0x55, `--debug_affinity_logical_processors=4`. A = a warning on every call, B = once. Mean over the 3 runs; the difference B - A with its 95% interval (Welch).

| Group P: mask 0x55, nothing else running | A | B | B - A |
|---|---|---|---|
| warnings a run | 108,987 | 1 | -109,000 |
| warnings a second while driving | 1,060 | 0 | |
| log file | 11.3 MB | 0.2 MB | |
| audio pump round, mean | 742 us | 726 us | -15.5 us (-98 to +67): noise |
| pump rounds that started 8 ms or more late, per 10 s | 995.1 | 994.4 | noise |
| silent audio frames a minute | 0 | 0 | |
| frame time, median / 99th percentile | 33.32 / 34.08 ms | 33.33 / 34.09 ms | noise |
| process CPU while driving | 0.707 cores | 0.680 cores | -0.027 (-0.134 to +0.080): noise |
| audio pump round, longest | 3,028 us | 3,673 us | +645 us (+63 to +1,230): outside the noise |

| Group S: 0x55 and 4 busy processes | A | B | B - A |
|---|---|---|---|
| warnings a run | 111,667 | 1 | |
| warnings a second while driving | 1,052 | 0 | |
| audio pump round, mean / longest | 3,785 / 68,343 us | 3,769 / 68,421 us | noise |
| pump rounds that started 8 ms or more late, per 10 s | 281.9 | 295.2 | noise |
| silent audio frames a minute | 185.8 | 190.4 | noise (-260 to +269) |
| frame time, median / 99th percentile | 32.40 / 110.07 ms | 32.42 / 112.06 ms | noise |
| process CPU while driving | 1.022 cores | 1.051 cores | noise (-0.020 to +0.078) |

With the 4 busy processes the game could not hold its 30 FPS cap (25.9 and 26.3 FPS), so the emulation of a slow CPU worked, and the old behaviour's 1,052 warnings a second made no difference to it.

Reading: the warning count fell from about 109,000 a run to 1, as intended, and nothing the player feels changed: the frame time, the audio pump, the silent audio frames and the process's CPU are inside the run-to-run noise in both groups. One measure was outside it, the longest single pump round without the busy processes (3.0 against 3.7 ms, B longer): it is one of 16 measures of the group, the change gives no reason for it, and it is read as chance, not shown. So this is log hygiene, not a speed-up, and the flood is not what makes the audio stutter on the test PC; the reporter's cause is still open. What it cannot show: 3 runs a side is a coarse test (the intervals above are the limit of what it can see); one machine with cores two to three times as fast as a 3rd-generation i5's; the busy processes are an emulation; the sound was not listened to; Direct3D 12 only. Linux and the PS5 build: not built or run.

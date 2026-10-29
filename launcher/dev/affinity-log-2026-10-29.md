# The "too few processor cores" warning, once per run (issue #21, 2026-10-29)

**State: the patch is written and checked to apply; the chain below has not run, so nothing here about the cost of the flood is measured.** Linux and the PS5 build were not built or run.

## What the report showed

Fork issue #21: an i5 of the 3rd generation (4 cores, 4 threads), an RX 580, v0.3.0, 30 FPS cap, audio and music stutter while the car moves. Its log (two parts, 57 s of play):

- "Too few processor cores - scheduling will be wonky": 58,797 lines, 1,000 to 1,150 a second, 54,134 from one game thread.
- Audio: 70 to 80 underruns a second at the worst; the audio pump's rounds took 4 to 9 ms (`pump: avg_us`); on the test PC (Ryzen 7 9800X3D, 16 logical processors) a round takes 0.014 ms, and 1 to 1.5 ms with four busy processes sharing four of its logical processors.
- The 1,805 "audio underrun in the middle of the game's audio" lines are the audio stats' own (`--audio_stats`, which that user was asked to turn on), one per run of silence.

## Where the line comes from

`XThread::SetActiveCpu` (`sdk/src/system/xthread.cpp`) is called by the game's `KeSetAffinityThread` (`xboxkrnl_threading.cpp`). With `std::thread::hardware_concurrency()` below 6 it logged the warning instead of setting the affinity, on every call. With 6 or more it sets the affinity only if `ignore_thread_affinities` is off, and that is on by default, so no PC applies the game's affinities by default: the warning only ever described a difference that had no effect.
A warning-level line is written to the rotating log file and flushed (the log flushes at info), and sent to the debugger channel, under the sinks' mutex, which every thread that logs takes.

## The change

`patches/rexglue-affinity-warning-once.patch` (SDK, `src/system/xthread.cpp`):
- `affinity_warning_once` (default true): the line is logged once per process. Its text now says what it means. `--affinity_warning_once=false` is the old behaviour (every call).
- `debug_affinity_logical_processors` (default 0): only `SetActiveCpu` reads it. N makes it behave as if the PC had N logical processors. The test PC has 16, and the earlier 4-core tests limited the game with an affinity mask (a job object), which does not change `hardware_concurrency()`: the warning path never ran in them.
- Everything else that counts processors is untouched (`mcla_small_cpu_defaults` reads the mask and the topology itself; the pipeline creation threads use `logical_processor_count()`).
- Other per-call logs on that path: none. Everything else in `xthread.cpp` and `xboxkrnl_threading.cpp` at warning or above reports a failure.

## The chain (not run yet)

`C:\mcv\affinitylog\chain\chain.ps1`, one exclusive hold of at most 40 minutes: the branch `fix/affinity-warn-once` built once (fast-setup), then runs on one build, scratch roots only, Direct3D 12 on the RTX 5080, native resolution, `--mcla_fps=30`, `--audio_stats=true --audio_stats_interval=1`, `--debug_affinity_logical_processors=4`:

- group P: affinity mask 0x55 (4 logical processors on 4 cores, one logical processor each, the shape of the reporter's PC);
- group S: the same plus 4 busy processes in the game's job object, normal priority (an emulation of a slow CPU, as `audio21` did);
- A = `--affinity_warning_once=false` (a warning on every call), B = the default (once); 4 runs of each in each group, in the order A B, B A, A B, B A.

Each run: boot to the city, stand 6 s, the car drives for 30 s (the measured part). Per run: the warning count (whole run and the driving part), the audio stats of the driving part (pump `avg_us`, `max_us`, late (> 8 ms) and over 4 ms rounds, the SDL callbacks' underruns and steady underruns), the frame times (median, p99, mean FPS), the process CPU (per-thread times once a second: cores used, ms per swap, the busiest threads), the size of the log. A run whose car stood still is not used (the position is logged during the drive). The statistics are per group: mean and range of each side, the difference of the means with a 95% interval (Welch); with 4 runs a side the intervals are wide, and the report says when a difference is inside them.

Results land in `C:\mcv\affinitylog\out` (see `C:\mcv\affinitylog\NOTES.md`).

## Results

Not run yet. This section is to be filled from `C:\mcv\affinitylog\out\REPORT.txt`; until then the docs say "not measured".

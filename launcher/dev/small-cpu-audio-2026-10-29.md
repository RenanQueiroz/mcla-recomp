# 4 cores without SMT and two audio options join the small-CPU defaults (v0.3.1, 2026-10-29)

**State.** Decided by the owner on 2026-10-29 for v0.3.1, after the `audio21` chain (below). Implemented in `ApplyCpuDefaults` (`src/mcla_app.cpp`, Windows only). The chain that
checks the new build's log lines and option values is `smalldef` (results in the work folder; its numbers are added here when it has run: "Check of the build" at the end).

## What changed

1. `mcla_small_cpu_defaults` (on) now also applies to **4 logical processors that are 4 cores without SMT**, and to 4 logical processors whose cores cannot be told. v0.3.0 left both out
   (`small-cpu-2026-10-28.md`, "Left out"). With the exclusion gone, the core count is only printed in the `CPU:` line; no rule depends on it.
2. On those CPUs (5 or fewer logical processors, the switch on) the game also sets two SDK options as defaults: `audio_thread_priority = 3` (the audio worker and the XMA decoder
   thread join the Windows multimedia scheduler, task "Pro Audio") and `audio_adaptive_queue = true` (after an underrun the mixer runs 4 frames further ahead, 21 ms, up to 24
   frames, 128 ms, instead of the usual 8 frames, 42.7 ms; nothing is added until an underrun happens). Both come from `rexglue-xma-decoder-fixes.patch` and were off before. They
   are set as defaults the way `mcla_poll_wait` is: a value from the command line, the environment or `mcla.toml` wins, nothing is saved.
3. The `CPU:` line lists them. Example: `CPU: 4 logical processors on 4 cores: timer queue high-resolution wait on, pump wake on demand on, poll wait on, audio thread priority 3,
   audio adaptive queue on (automatic; 5 or fewer, with mcla_small_cpu_defaults)`; with `--audio_thread_priority=0` it says `audio thread priority 0 (set by the player)`.
4. Off switches: `--mcla_small_cpu_defaults=false` takes back all of it (only the high-resolution wait is then on, as in v0.2.x), `--audio_thread_priority=0` and
   `--audio_adaptive_queue=false` take back the audio part alone.
5. Not changed: 6 or more logical processors; Linux and the PS5 (`ApplyCpuDefaults` is inside `#ifdef _WIN32`; the PS5 host `ps5/game/main_ps5.cpp` does not call it and sets no audio
   option; the SDK's own defaults for the two options stay off).

## Evidence (chain `audio21`, 2026-10-29)

Both runs used the shipped v0.3.0 build (8f84971) with the options set on the command line, not the new build. RTX 5080, Direct3D 12, native resolution, windowed, the default 30 FPS
cap, the process limited by affinity to 0x55 (4 logical processors = 4 cores, one logical processor each) on a Ryzen 7 9800X3D. Variants: S = v0.3.0 as shipped (high-resolution
wait only); A = S + `--audio_thread_priority=3 --audio_adaptive_queue=true`; C = the small-CPU options forced on (`--mcla_poll_wait=true --mcla_pump_wake_on_demand=true`, what
`mcla_small_cpu_defaults` does elsewhere), with `--mcla_pump_wake_pause_s=15`; CA = C + A (what the new default gives). The route: boot, free roam, 8 s standing, 24 s driving at
cap 30, 10 s driving uncapped, the pause menu. "Stress" = 4 busy processes on the same cores, an emulation of a CPU too slow for the game, not a real one.
**Run 1** (the car was stuck against a wall 33 units from the spawn in every run, so its audio figures are not about driving; its frame times are of the same scene): S 5, A 5, C 5, CA 5
runs at 0x55; 0xF (2 cores with SMT) as a control, 3 runs a side; stress S and A 3 runs a side. **Run 2** (a route that works: the car moved 15.5 units/s at cap 30 and 21 units/s
uncapped; 6 of 14 runs had one 2-second window under 15 units): S 4, A 4, C 4, CA 2 (the hold ran out of time for two more); stress S and A 3 runs a side.

| | Run 1 (car stuck) | Run 2 (car drives) |
|---|---|---|
| Uncapped frame, C against S | 6.90 to 4.14 ms (-39.9%), acting in 5 of 5 | 6.94 to 4.48 ms (-35.4%), acting in 4 of 4 |
| Uncapped frame, CA against S | 6.91 to 4.16 ms (-39.9%), 5 of 5 | 6.92 to 4.41 ms (-36.2%), 2 of 2 |
| Cap 30, C: frame, p99, CPU per swap | -0.0%, -1.1%, +3.5% | -0.0%, +0.4%, +3.9% |
| Cap 30, CA: frame, p99, CPU per swap | -0.0%, -0.3%, +0.1% | -0.0%, +0.1%, +2.7% |
| Watchdog trips of the wake on demand (C / CA) | 0 / 0 | 0 / 0 |
| Silent audio frames in the drive segments, unloaded (S / A / C / CA) | 0 / 0 / 0 / 0 (car stuck) | 0 / 0 / 0 / 0 |
| Uncapped p99 tail (rule: a run above 1.5 times the S maximum) | C none; **CA 1 of 5 (20.5 ms; S 8.1 to 8.2 ms)** | C none, CA none of 2 (S itself ranged 9.7 to 28.5 ms) |
| Cost of A on S (frame, p99 uncapped; cap 30) | PASS (+0.2%, -0.0%) | PASS (-0.4%, -9.1%) |
| Cost of A on C (CA against C), uncapped p99 | **FAIL: +68.4%** (the one 20.5 ms run), frame +0.3% | LOOK: +4.6%, frame -1.6% |
| Stress, S: silent frames in the drive segments | 31, in 2 of 3 runs | 184, in 3 of 3 runs |
| Stress, A | 4, in 1 of 3 | 3, in 1 of 3 |
| Stress, adaptive queue growth (A) | 12.0 frames a run (64 ms) | 14.7 frames a run (78 ms) |

Control, run 1, at 0xF (2 cores with SMT, where v0.3.0 already turned the small-CPU options on): A against S and OA against O (the options with the small-CPU defaults off) cost
PASS; small-CPU options on against off: uncapped frame -36.1%, p99 -35.3%. The adaptive queue grew 0 frames in every run without stress.

What this says, and what it does not:

- **The speed-up is the one of the other small CPUs**: 35 to 40% off the uncapped frame at 4 cores without SMT, in 9 of 9 runs with the options acting (C) and 7 of 7 (CA), the default
  30 FPS cap unchanged (frame -0.0%, CPU per swap within +4%).
- **The earlier reason for the exclusion** (`small-cpu-2026-10-28.md`): in the first test, 2 of 5 runs with the options on had a short audio dropout during the city load (21 and 23 s
  into the run) that made the watchdog pause the wake on demand, and 1 of the 3 good runs had a tail of long frames (p99 21.7 ms free roam, 29.5 ms driving). **Run 2 and run 1
  partly answer it**: with the same boot, C ran with the wake acting in 9 of 9 runs and CA in 7 of 7, with 0 watchdog trips and no silent frame in either run. That is 0 of 16 against
  2 of 5 (counting C and CA together), which is not a proof (for C alone, 0 of 9 against 2 of 5, one-sided Fisher p = 0.11); the cause of the dropouts was never found and the earlier
  chain ran a pre-release build. The watchdog (first trip: the wake pauses; second: off for the session) stays as the safety. **The tail is not answered**: no tail in 9 C runs, but
  1 of the 7 CA runs (run 1, uncapped part only) had one, 20.5 ms against 8.1 to 8.2 for S. It is the same kind of event as the 21.7 ms of the first test; 1 of 7 against 0 of 9 is not
  separable, and whether the audio options cause it is not known. It does not show at the default cap (p99 within +0.4%).
- **The audio options do something only under starvation.** Unloaded, S had no silent frame in the drive segments (4 runs in run 2, where the car drove, and 5 in run 1, where it was
  stuck), so issue #21's stutter was **not reproduced** on the unloaded 4-core mask and the options could not be shown to remove it. Under the stress, S played 184 and 31 silent
  frames in the drive segments (3 of 3 and 2 of 3 runs), A 3 and 4 (1 of 3 in each). The adaptive queue grew only there (12 to 15 frames) and not in any unloaded run. Under the stress the frame was not slower with A (drive +0.3% and -0.2%;
  uncapped -12.0% and -11.0%). **Not measured: the stress with C or CA**, which is the combination the new default gives; and the stress is a CPU that cannot hold 30 FPS (the busy
  processes took the game to 0.8 to 0.9 cores, 19 to 22% of the frames slow), not a real slow CPU.
- **Fork issue #21** (an i5 of the 3rd generation, 4 cores and 4 threads, 8 GB, RX 580, the default 30 FPS cap: audio and music stutter while the car moves) is the real-world case for
  all of this. **It was not reproduced on real old hardware**: the test PC is a fast CPU with 96 MB of L3 limited by affinity, the GPU is an RTX 5080 (the reporter's RX 580 is a
  different load), and nobody listened to the sound. The defaults are a measured speed-up plus an emulated-starvation protection, not a confirmed fix for that report.
- **Latency.** The adaptive queue adds up to 86 ms (24 frames instead of 8) of output delay after the queue has run dry, and only then; with MMCSS the audio threads are scheduled ahead
  of ordinary ones. Neither was heard.

## Not covered

One machine; Direct3D 12 only (a Radeon on Vulkan is the reporter's likely case); the stress with C and CA; a real 4-core CPU without SMT (including its efficiency-core cases: a core
with one logical processor counts as a core); the audio by ear; Linux and the PS5 (not built or run: the code is Windows only); 3 or fewer logical processors (these get the defaults
as before, and now the audio options too, which no run covers).

## Check of the build

(The chain `smalldef` builds this branch with `fast-setup.ps1` and checks the `CPU:` and `PC defaults:` lines and the option values at 0x55, 0xF and all threads, and at 0x55 with the
player's values. Result lines are added here after it has run.)

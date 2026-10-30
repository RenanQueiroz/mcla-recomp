# The game's audio mixer thread at normal priority (fork issue #21, 2026-10-30)

**State.** `audio_mixer_priority` (`src/audio_mixer.cpp`) is on branch `fix/audio-mixer-priority`, not merged. It is set to 3 by `mcla_small_cpu_defaults` (5 or fewer logical processors,
Windows). The lab results below were measured with a lab build (`diag/audio-pump-cpu`, never released). The shipped build's own runs are the chain `mixerfix` (13 runs); its table is in
"The shipped build" and is **empty until that chain has run**. Issue #21 is **not shown to be fixed**.

## What the audio does (read from the recompiled code, then seen in the lab runs)

- The SDK's "Audio Worker" thread runs the game's callback (`sub_823F56D0`) once per 5.33 ms frame. The callback's helper `sub_823ECA40` sets an event (`0x828CD9AC`, at `0x823ECA90`) and
  waits for another one (`0x828CD99C`).
- The thread that answers is a guest thread with entry `sub_823EC8C8`: it waits for the first event, mixes the frame (`sub_823EC160`, `sub_823EC248`), sets the second (`0x823EC978`) and waits
  again. Created with flags `0x10000001`, 256 KiB of stack, in the same millisecond as the callback is registered, by the same thread; it starts about 0.25 s later.
- The game asks for priority +15 for it (`KeSetBasePriorityThread`). The SDK ignores that (`ignore_thread_priorities`, default true; with it false a request of 15, 16 or 17 still maps to
  normal), and on a PC with fewer than 6 logical processors it applies no affinity either. So the thread runs at normal host priority with the render, GPU and job threads.
- Lab, 4 logical processors on 4 cores, nothing else running: the callback's round is 738 microseconds wall, 8 of them CPU; the mixer's round is 725 wall and 721 CPU. With 4 busy processes on the
  same cores: callback 3,773, mixer 3,702 wall for 714 CPU, woken 200 microseconds after the pump's signal (29 to 456). The callback waits; the mixer is the one that is not given a CPU.

## Lab results (2026-10-30, Ryzen 7 9800X3D limited to 4 logical processors on 4 cores by a job object, RTX 5080, Direct3D 12, 30 FPS cap, audio21's driving route, 24 s measured, mean [min..max]; "separated" = every run of one side beyond every run of the other)

| 4 busy processes, n = 3 a side | option off | `audio_mixer_priority=3` (host priority highest + MMCSS) |
|---|---|---|
| silent audio frames in the middle of the audio, per run | 12.3 [4..29] | 0 [0..0], separated |
| mixer woken after the pump's signal (us) | 200 [29..456] | 3 [3..3], separated |
| callback round, wall (us) | 3,773 [1,441..4,955] | 2,290 [2,178..2,346] |
| mixer busy (wall, minus its kernel waits) / CPU (us) | 3,702 / 714 | 2,159 / 727 |
| frame median (ms) | 32.34 | 32.16 |
| frame p99 (ms) | 156.9 [50..229] | 91.4 [73..101] |
| process CPU (cores) | 0.88 | 1.09, separated |
| main thread CPU (% of a core) | 20.2 [12.9..24.9] | 32.9 [32.3..33.6], separated |

Without the busy processes (1 run a side): callback 738 against 724, mixer woken after 6 against 5 microseconds, no silent frame either way, no change. The game's own priorities
(`--ignore_thread_priorities=false`, n = 3): callback 4,889, 24.3 silent frames a run: no help. The denormal counters of the mixer thread were 0.04 flagged stretches a round: not it on this route.

**Why the main thread's CPU rose** (looked at, not explained). It is not more frames: 694 to 699 frames in the 24 s with the option on; with it off 719, 590 and 609 (the last two fell
behind the 30 FPS cap). The main thread used 11.3 ms of CPU a frame with the option on, 4.0 in the plain run, 4.3 in the off run that held the cap and 9.3 and 9.8 in the two that did not; its
Sleep time (the game's Sleep wrapper at `0x824602C4`, caller unknown) was 160 and 229 ms a second in those two and 2 with the option on. So under this load the main thread burns more CPU a
frame whatever the audio does, and more of it with the mixer boosted. A boosted mixer takes CPU from the other game threads first, so waits for them may be longer and spin. Not measured further.

**What the mixer still waits for.** With the option on the mixer was not on a CPU for 1,433 of its 2,159 busy microseconds a round. 104 to 149 of them were in the one guest critical section it
enters each round; none were in a kernel wait (those are not "busy"). The rest could be a priority inversion on a host lock whose holder the busy processes starve (the SDK's global critical
region, which the guest's kernel calls take), page faults, or a spin in the game's code. Nothing measured tells which. A Windows trace (WPR or xperf, context switches with the ready-thread
stacks) of a boosted run would; no other thread has evidence for the same treatment, so none got it. `audio_client_lock` is about the audio client table, not the mixer.

**The reporter's PC.** The report has the callback at 10.6 ms a round while driving (period 5.33 ms). A core 2.5 to 3 times slower than the test PC's predicts about 2 ms of CPU for the mixer
(720 microseconds times 2.5 to 3), so a boosted mixer alone does not explain 10.6 ms. The busy processes are an emulation, not an i5 of the 3rd generation with 8 GB and an RX 580.

## "Found in 10 of 11 runs" and the second thread F8000530

Both come from the lab report keying the mixer by its handle. In all 11 runs exactly one thread was created with entry `0x823EC8C8` (none with `0x823EC990`), with the same flags (`0x10000001`),
stack and processor byte (16), and the lab's own "mixer thread found" line (it goes by the signal) is in all 11. In run SG2 the handle was `F8000530` instead of `F8000534`: the game created
one small kernel object fewer before it, so the handle numbers shifted by 4. The report counted `F8000530` as another signaling thread and the run as a miss. A handle is not a way to find the
thread; the entry address is, and it held in 11 of 11. `audio_mixer_priority` uses the entry.

## Stress runs that lingered 180 s

In the lab chain's first hold the stressed runs took 262 s instead of 90: the game logged "Title terminated; hard-exiting process" and devrun then waited until its 230 s timeout with the busy
processes alive. The second hold started the busy processes when the log showed `mark free_roam` and ended them when it showed "Window closing" or "hard-exiting": all 11 runs, 9 of them with
busy processes, exited with code 0 after 77 s, the batch took 871 s. The `mixerfix` chain uses the same watcher and also checks, for every run, the exit code, the three
shutdown lines and the time the process outlived its last log line (at most 10 s, a normal exit is a few milliseconds).

## What the shipped build does

- `audio_mixer_priority` 0 to 3 (restart), default 0; `mcla_small_cpu_defaults` sets 3 with 5 or fewer logical processors (Windows) next to `audio_thread_priority` 3 and `audio_adaptive_queue`.
  Log: the `CPU:` line ends its option list with `audio mixer priority 3`, and `audio mixer thread (guest entry 823EC8C8): host priority highest, MMCSS joined (task "Pro Audio", index N) (level 3)`
  when the thread starts. If audio_mixer_priority is above 0 and no thread with that entry has started when the pump has gone round 50 times, a warning says so.
- The game's code finds the thread: `sub_823EC8C8` is overridden to call the SDK's `RaiseCallingAudioThread` and then the game's own body. The SDK patch (`rexglue-xma-decoder-fixes.patch`)
  has the host calls and no game address. Linux and the PS5 build: the option does nothing and the SDK call returns false (not built or run there).
- `--audio_stats=true`: the stats lines end with `| mixer: rounds=N busy_avg_us=A busy_max_us=B cpu_avg_us=C wake_avg_us=D wake_max_us=E`. Busy is the mixer's wall time from its wake-up to its
  answer to the pump (any wait inside the round is in it); CPU is `QueryThreadCycleTime` over the same stretch; wake is the delay from just before the pump's `KeSetEvent` to the mixer's return
  from its wait. The three hooks (`config/audio_mixer.toml`) do nothing but a relaxed load without `audio_stats`.
- How to read a report's line: busy about equal to CPU and a callback round (`pump: avg_us`) near 0.75 ms is a mixer that computes; busy far above CPU, or wake-ups of hundreds of
  microseconds, is a mixer that waits to run (on that report, `audio mixer priority` should say 3).

## The shipped build: the chain `mixerfix`

13 runs in one exclusive hold of at most 20 minutes (D3D12, 30 FPS cap, `--audio_stats`): A = `--audio_mixer_priority=0` (v0.3.2 behaviour), B = the new default; 4 busy processes (SA, SB, n = 3
alternating A B B A A B), 16 logical processors with the default (FB1), plain (PA, PB, n = 3). `C:\mcv\mixerfix\chain`; results in `C:\mcv\mixerfix\out\VERDICT.txt`. Run on 2026-10-30; 9 of the 13 runs
used: SB3's car got stuck on the route (not usable; the game exited normally), and the deadline dropped PA2, PA3 and PB3.

| Driving | 4 busy processes: A (n = 3) | B (n = 2) | Plain: A (n = 1) | B (n = 2) |
|---|---|---|---|---|
| Silent frames in the middle of the audio, per run | 5.0 (0 to 15) | 0 | 0 | 0 |
| Mixer wake-up after the pump's signal, mean (us) | 26.7 | 3.2 (separated) | 4.9 | 4.6 |
| Worst wake-up (us) | 25,945 | 114 (separated) | 250 | 30 |
| Pump round (us) | 3,684 | 1,822 | 704 | 719 |
| Frame median / p99 (ms) | 32.9 / 130 | 31.2 / 110 | 33.3 / 34.1 | 33.3 / 34.1 |
| Process CPU (cores) | 0.80 | 1.15 (separated) | 0.69 | 0.73 |

With the lab build's runs (n = 3 a side: 12.3 silent frames a run against 0), the option on had no silent frame in 5 runs and the option off had some in 5 of 6. Under stress the process uses more CPU with it on (+44%, the main thread's share not explaining it all); frames were not slower. Cause not known. 16 logical processors with the default: the option is 0, no mixer line, the stat works, 0 silent frames. All 9 runs exited with code 0, within 0.5 s of the last log line.

## Not covered

One PC; busy processes are not an old i5 with 8 GB and an RX 580; a mixer thread boosted here has the PC's spare cores to run on; the audio is counted, not listened to; Vulkan, Linux and the
PS5 were not run; the build here was compiled by CI only (no local build by the author of this note).

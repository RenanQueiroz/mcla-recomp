# The audio pump takes 10.6 ms a round on a 3rd-generation i5 (issue #21, 2026-10-30)

**State: the CPU-time stat (`cpu_avg_us`, `cpu_pct` in the `pump:` part of the `audio_stats` line) is written and compile-checked by CI. The lab counters below are on the branch `diag/audio-pump-cpu` only. The first lab run (2026-10-30, "Results") found that the pump's callback waits for another thread and does no floating point itself, so it said nothing about denormals: the mixer thread was not measured. The third commit (`rexglue-lab-thread-diag.patch`) finds that thread and measures it; it is compile-checked by CI and not run yet. Linux and the PS5 build were not built or run.**

## What the report shows

Fork issue #21, log of v0.3.2 (`mcla_002.log`): an i5 of the 3rd generation (Ivy Bridge: 4 cores, 4 threads, no AVX2), 8 GB, an RX 580, 30 FPS cap. Audio is choppy while driving and fine when paused or standing. The small-CPU defaults (MMCSS audio priority 3, adaptive queue, pump wake on demand, poll wait) are on in that log and changed nothing.

- The audio pump (the guest mixer callback, `function_dispatcher_->Execute` in `AudioSystem::WorkerThreadMain`) takes 10.6 ms a round while driving; its period is 5.33 ms. So only 50 to 82% of the audio is made (938 to 1,540 of 1,875 frames per 10 s). 0.8 ms at the title, 2.3 ms while loading.
- XMA decoding is about 60 us a kick at about 1,300 kicks a second, about 8% of the time: not the cause.
- Here (Ryzen 7 9800X3D limited to mask 0x55, driving at cap 30, chain `affinitylog`): 0.74 ms; with 4 busy processes 1.5 to 3.9 ms. A CPU 2.5 to 3 times slower explains about 2 ms, not 10.6 (14 times). The worker has MMCSS priority, so ordinary preemption should not explain it either.

## Two readings

- **A. The round waits.** A guest lock or wait held by a lower-priority or starved game thread (priority inversion), or a spin on one (`RtlEnterCriticalSection` spins `spin_count` times on a compare-and-swap before it waits, and that spin is CPU time, not waiting).
- **B. Denormal floats.** Scalar floating point keeps denormals (MXCSR 0x1F80), the vector (VMX) code flushes them (0x9FC0). Filters and envelopes that decay to denormals cost 150 or more cycles an operation on Intel CPUs before Skylake, and little on a Zen. That would explain why the time cannot be reproduced here. Unproven, and may be wrong.

Neither is assumed. A third is possible and not looked at: paging or I/O waits on an 8 GB PC (they would show as a low `cpu_pct` too).

## What the stat says (first commit, for a release)

`audio_stats` now ends the `pump:` part with `cpu_avg_us` (the worker's CPU time per callback) and `cpu_pct` (that as a share of the callbacks' wall time). A reporter's next log then says directly: low `cpu_pct` (the callback waited or was not scheduled) or near 100 (it computed, or spun on a lock). Which clock: `QueryThreadCycleTime` around every callback (exact, whatever the length), turned into time by a factor measured at start (three 8 ms spells against the performance counter, largest ratio). `GetThreadTimes` moves in clock ticks (15.6 ms on this PC, checked: 78.12 ms for an 80 ms spell, 0 for 0.5, 2 and 5 ms spells), too coarse for a 1 ms callback; over a 10 s sum it is right to a few percent but says nothing about the single rounds. On this PC the cycle counter runs at the time-stamp counter's rate (4,681 cycles per microsecond measured, 4,691 MHz in the registry). On the reporter's PC the factor is measured, not assumed, and the line `audio stats: pump CPU time from QueryThreadCycleTime (N cycles per microsecond, measured now)` in the log says what it found.

## What the lab commit adds (second commit, never released)

`patches/rexglue-lab-mxcsr-diag.patch`, with `--audio_stats --audio_mxcsr_diag`: an `audio diag` line after each interval line (see the patch's section in `patches/README.md`). Per audio callback: MXCSR writes; stretches between writes by mode; stretches that ended with the denormal-operand flag (DE) or the underflow flag (UE); the time in the scalar and the vector mode (tells whether the guest mixer is scalar floating point or VMX where its time goes); the time in guest critical sections and waits. For scale: the three guest threads with the most MXCSR writes.

What the counts mean: a stretch with DE in the scalar mode consumed at least one denormal operand, so the count of such stretches is a lower bound on the denormal operations. A Zen CPU raises the same flags, so the counts are valid on this PC though the time penalty is not. The vector mode has DAZ and FZ set: its denormals are flushed on any CPU at no cost, and UE there counts flushed results.

## Prediction for a 3rd-generation Intel CPU (to fill in after the runs)

lower bound = (DE + UE stretches in the scalar mode per callback) x 150 cycles / the clock (3.3 GHz is a typical i5-3xxx); upper bound = (time in scalar stretches with DE or UE, in cycles) / 2 cycles per operation x 150. Compare with the excess of the reporter's round over this PC's (10.6 ms minus the time here, scaled for the slower CPU). Uncertainty: 150 cycles is the usual figure for a microcode assist, not measured on an Ivy Bridge; the stretches are not operations (the lower bound is one per stretch, the upper assumes every operation in the stretch is denormal at 2 cycles each).

## Method (chain `audiopump`, scripts kept outside the repository)

One build of the branch with its own SDK (`fast-setup.ps1 -NoBundle`), then runs on that one build: scratch save and shader storage, Direct3D 12, windowed, `--mcla_fps=30 --audio_stats=true --audio_stats_interval=1 --audio_mxcsr_diag=true`, affinity mask 0x55 by a job object (4 logical processors on 4 cores). The route is audio21's `au-ab.pad` (stand, then a 24 s drive on the street that `follow` drives), the same as `affinitylog`. Group P: plain. Group S: 4 busy processes in the game's job object. The per-second lines are cut into the standing and the driving part by the pad's marks.

## Results

### Run 1 (lab commit 61b5302; this PC, Ryzen 7 9800X3D limited to mask 0x55, Direct3D 12, cap 30, v0.3.2 defaults; n=2 a group)

Per audio callback while driving (the pump's round on the audio worker): group P (plain) 748 us wall, 7 us CPU (0.9%), 0 MXCSR writes, 1 guest critical section taking 0 us, 1 guest wait taking 742 us (99% of the round); 11 XMA kicks a round. Group S (4 busy processes in the game's job object) 1,883 us wall, 7 us CPU (0.4%), 1 guest wait taking 1,877 us; 5.9% of the rounds over 4 ms. The CPU time did not grow with the load: all of the growth (1,136 us) is time off the CPU. (Measured; two runs a group, the third of each did not start before the hold's deadline; two attempts of S1 were dropped because the game had not left the title screen after 90 s with the busy processes running.)

What that says: the callback is **not** computing. It starts a wait and sleeps in it for nearly its whole round, so what takes the time is the thread (or threads) it waits for. Raising the audio worker's host priority (`audio_thread_priority`) cannot shorten a wait for a thread that has not been raised, which fits the reporter's finding that it changed nothing (inference).

**Correction.** The first version of the report said "B (denormals) CANNOT explain it". That was wrong as worded: the counters were on the callback's thread, which makes no floating-point calls (0 MXCSR writes), so they say nothing about the guest mixer. The denormal question is open until the thread that mixes is measured (`rexglue-lab-thread-diag.patch`, its `mixer` line). The Q3 arithmetic in the report was done with zero denormal stretches on that thread and is void.

### Run 2

Not run yet.

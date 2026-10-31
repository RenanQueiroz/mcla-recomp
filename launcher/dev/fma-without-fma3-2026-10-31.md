# Fused multiply-adds on a CPU without FMA3 (fork issue #21, 2026-10-31)

Branch `fix/fma-without-fma3`. Not merged. Nothing here was run on a CPU without FMA3.

## The question

Fork issue #21 (Intel Core i5-3470, Ivy Bridge, 4 cores, 4 threads, 3.4 GHz, v0.3.3, 30 FPS cap): while driving, the game's mixer thread (guest entry
`0x823EC8C8`) computes for about 9,300 microseconds of CPU in each 5.33 ms audio round (the audio stats line, `| mixer: ... cpu_avg_us=`); only 50 to 60% of the
audio is made. A Ryzen 7 9800X3D (4 logical processors, driving) needs about 700 microseconds: 13 times less, where the CPUs differ by 2.5 to 3 in general.

## What the reporter's log says (verified, `scripts` of the scratchpad `iss21/stats.py` and a per-interval table)

The mixer's CPU per round grows in a straight line with the XMA kicks per round: `cpu_us = 1230 + 563 x kicks` on the reporter's PC (11 ten-second intervals), `170 + 48 x kicks`
on this PC (56 one-second intervals). The XMA decode itself is 56 microseconds a kick there and 15 to 16 here (3.5 times, the general gap). Everything else a kick causes is
about 507 microseconds there and 33 here: 15 times. The extra cost is per voice, not per round.

## The C library's fma (verified)

- The generated code writes every scalar fused multiply-add (fmadds, fmsubs, fnmsubs, fnmadds, fmadd, fmsub, fnmsub) as `std::fma(double, double, double)`: 9,118 sites, 8,675 of them
  the single-precision forms `double(float(std::fma(...)))`. The vector forms (vmaddfp, vnmsubfp) are not fused (SIMDe multiply and add) and cost the same on every CPU.
- The SDK compiles for x86-64 with `-msse4.1` only (`cmake/rexglue_helpers.cmake`), so each is a call. `mcla.exe` imports `fma` from `api-ms-win-crt-math-l1-1-0.dll`
  (ucrtbase.dll; llvm-mingw 20250709, UCRT).
- ucrtbase's `fma` (Windows 11 10.0.26100.9711, disassembled): compare a flag with 0; with FMA3 `vfmadd213sd` and return; otherwise a jump into a long software routine (about 1,500
  lines of SSE and integer code, `_dnorm`, `stmxcsr`). The flag is set at start from the CPU and is what `_set_FMA3_enable` changes.
- Microbenchmark (`launcher/dev/fma-lab/bench_fma.c`, built like the game, Ryzen 7 9800X3D at 5.24 GHz, 10 million calls each):

| `fma` | independent calls | dependent chain |
|---|---|---|
| FMA3 path | 1.15 ns (6 cycles) | 2.48 ns (13 cycles) |
| software path (`_set_FMA3_enable(0)`), ordinary operands | 30.0 ns (157 cycles) | 30.5 ns (160 cycles) |
| software path, c = 0 or a = 0 | 3.5 ns (18 cycles) | 3.7 ns (19 cycles, a = 0) |
| fast path of `src/fma_fast.cpp` | 1.76 ns (9 cycles) | 3.05 ns (16 cycles) |

On an Ivy Bridge the software routine takes more cycles than on this wide core (my assumption: 1.5 to 2.5 times; not measured).

- Sampling profiles of the mixer thread (an older build, 16 logical processors, free roam and a race, `cpu-profile` of the scratchpad): `ucrtbase.dll!fma` is 2.2% (roam) and 4.5% (race) of
  the thread's running time, called from guest `sub_82147738` and `sub_82146BE0` (168 fma sites between them; the second is 12% of the thread) and a few others. At 1.2 to 2.5 ns a call
  that is 20,000 to 30,000 calls a round (inferred from a sampling share). At 160 to 400 cycles (3.4 GHz: 47 to 118 ns) each, 1 to 3.5 ms of the reporter's 9.3. The chain counts the calls.

## The change

`src/fma_fast.cpp` defines `fma` in the executable (under the C++ name `mcla_fma_symbol` with the assembler label `fma`; a definition in the executable wins over the import library's thunk).
With FMA3 it calls the library's `fma`, as before. Without FMA3 (the library's `_get_FMA3_enable`) and with `fma_float_fast` on (default), a call whose `a` and `b` are exactly
single-precision values and whose `c` is finite is `a * b + c` in double precision: the product of two floats has at most 48 bits so it is exact, and the one addition is the one rounding of
the fused operation, in whatever rounding mode the code has set. Operands that are not floats, infinities and NaNs go to the library. Windows, llvm-mingw, x86-64 only.

`launcher/dev/fma-lab/fma_test.cpp` (the same file included, `-DMCLA_FMA_STANDALONE -DMCLA_FMA_TEST`) against `_mm_fmadd_sd` on this PC: 160,000,000 operand sets (random floats and doubles, moderate floats,
exact and near cancellation, wide exponent gaps, signed zeros, all floats incl. float denormals), rounding to nearest, down, up and zero, FTZ/DAZ off and on: the fast path applied to
159,601,341 of them (the rest are infinities and NaNs), **0 differences**.

## Measured with the library's FMA3 path forced off (lab build; THIS PC, not an Ivy Bridge)

Pending: the chain `ivy21` (C:\mcv\ivy21\chain) writes `C:\mcv\ivy21\out\VERDICT.txt`. Variants at 4 logical processors on 4 cores (0x55), cap 30, D3D12, the driving route of
`mixerfix`: A as shipped; B the library's FMA3 path off (`--mcla_lab_fma=nofma3 --fma_float_fast=false`: what an Ivy Bridge runs); C the same with the fix on; D A with the calls counted.
What B proves: that the software routine at the game's call count costs this much more CPU on a core that has it fast. What it does not prove: the figure on an Ivy Bridge.

## What a report from the reporter's PC would need to show

The `FMA:` line of the log (`runs its software routine (this CPU has no FMA3)`), and the mixer part of the audio stats line (`--audio_stats=true`) with and without
`--fma_float_fast=false`: `cpu_avg_us` should fall from about 9,300 by about the share this note estimates (1 to 3.5 ms) if the library's routine is the cause, and not move if it is not.

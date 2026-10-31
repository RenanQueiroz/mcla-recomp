# Line pipelines and sample positions on a Radeon (fork issue #28), 2026-10-30

Dev note. What was measured, how to repeat it, what is not known. The user-facing text is in [KNOWN-ISSUES.md](../../KNOWN-ISSUES.md) and [docs/off-switches.md](../../docs/off-switches.md); the change is `patches/rexglue-d3d12-msaa-line-guard.patch` (cvar `d3d12_msaa_line_guard`).

Test PC: Ryzen 7 9800X3D, integrated "AMD Radeon(TM) Graphics" (driver 32.00.21045.16010), RTX 5080, WARP, Windows 11 26300. **Only that one Radeon was tried.**

## The program

`launcher/dev/msaa-line-guard/msaa_line_pso.cpp`: one Direct3D 12 file, no game data, no dependencies but the SDK headers and `d3dcompiler`. Every case records one command list on a 1280x720 2x or 4x target (colour `R16G16B16A16_FLOAT` and `D32_S8` depth), binds PSOs in some order, draws two quads with slanted edges with a plain triangle PSO and reads back which samples each covered; from that it computes the sample positions (1/16 pixel). The Direct3D standard patterns are 2x (12,12) (4,4) and 4x (6,2) (14,6) (2,10) (10,14); "all samples at the pixel centre" is (8,8).

```
x86_64-w64-mingw32-clang++ -O1 -std=c++17 msaa_line_pso.cpp -o msaa_line_pso.exe -ld3d12 -ldxgi -ld3dcompiler -luuid -static
msaa_line_pso.exe AMD 2          (an adapter name part, then 2 or 4; "Basic" is WARP; REPRO_DEBUG=1 turns the debug layer on)
```

Output of the Radeon, RTX 5080 and WARP at 2x and 4x: `results-2x.txt`, `results-4x.txt` in that folder (the debug layer printed nothing on the Radeon or the RTX 5080).

## What it shows (Radeon; the RTX 5080 and WARP are standard in every case)

- A PSO with topology LINE and `MultisampleEnable` false, bound while the previous graphics PSO of the list has another sample count (or there is none: the first PSO of a list), leaves all samples at the pixel centre for the triangle PSOs after it. At 2x and 4x alike. A wireframe triangle PSO does the same. A point PSO, a 1-sample line PSO and a line-topology PSO with a geometry shader (the game's quad lists, also as wireframe) do not.
- Drawing a line or not, the line PSO's blend, depth, stencil, cull, a `clip()` or `SV_Position` in its pixel shader, and what happens in between (barriers, clears, binding the targets again, a compute PSO, a triangle PSO with `MultisampleEnable`) change nothing. A compute PSO first does not count as a graphics PSO. Pixel shaders that write `SV_Coverage`, force early depth tests or take interpolants still trigger it.
- The previous graphics PSO having the line PSO's sample count prevents it, whatever came before. A later PSO of the same count does not undo it; a PSO of another count does. That fits a driver that sets the sample positions when the count changes from one PSO to the next, and sets the centre for an aliased line PSO.
- Other cures: `MultisampleEnable` true on the line PSO; `SetSamplePositions(0, 0, nullptr)` after it.
- The guard (the patch): a triangle PSO with the line PSO's sample count, with one colour format or one depth format or both, no pixel shader, bound right before the line PSO. All 14 guard cases are standard at both counts. Not enough: a guard with no outputs (sample count 0) or 1 sample, or of the other count; the guard at the start of the list when the line PSO comes after a PSO of another count.
- The samples a line draw covers (`line px`) are the same with the guard as in the sequence that was never broken (a triangle PSO first). In the broken sequence they differ at 4x by 2 of 200 (sample 3), at 2x not.

## The capture replay (the game's frame)

The numbers behind the program were taken by replaying a captured flashing frame of the game with a D3D12 player: the second command list starts with the line PSO (a `DrawInstanced(24)` after binding the targets). Both samples drawn 261,744 pixels, one sample lost 133,905 (all with the second clear's colour), both 2,486. On the RTX 5080 and WARP the same stream loses 2,417 (edge pixels) and 10 to 19. A triangle PSO bound first in that list, or `MultisampleEnable` true on the line PSO, leaves 12 and 170 samples lost of 398,135, the noise between the RTX 5080 and WARP. The replay's work is in the scratch notes of chain `amd28g`; it is not in the repo (it holds game rendering).

## Open points

- **Four line PSOs of the capture did not trigger it** when bound alone before a probe (4168, 4170, 4244 and 4253; the other two, 4103 and 4179, did). Explained: the four have a geometry shader (they are the game's quad lists, which have the line topology type and are rasterized as triangles; the creation code of the capture's player shows it) and the two have none. The program agrees: a line-topology PSO with a geometry shader does not trigger it, with or without depth, stencil or wireframe (G12 to G14); the pixel shader's features do not matter (G8 to G11). The guard skips pipelines with a geometry shader.
- **A replay flashed where the live game had not.** The replay re-runs the list boundaries of the captured frame. The live boundaries move from run to run (`d3d12_submit_on_primary_buffer_end`, `d3d12_threaded_submission_split_draws`, the idle wait, timing), and with this driver rule a frame is hit only when a pipeline with a line topology is the first of a list, or follows a pipeline of another sample count. So the same game frame is clean in one run and hit in another. This is an inference from the program's rules; no live run was checked for it.
- **The reporter's RX 7800 XT** was not tried: another GPU generation and driver. If it does not show the collapse in the program, the guard may not be their fix.
- **The pixel-shader-interlock path** (Intel's default on Direct3D 12) builds pipelines with 1 sample; the guard leaves it alone and it was not tried.

## The idle wait, as evidence only

`d3d12_threaded_submission_idle_wait` stays as it is. If the live runs below show that with the guard on the integrated Radeon is as clean with the idle wait off as with it on, the idle wait was hiding the same list-boundary effect and could be retired in a later release; if it is not, it was fixing something else. That is for the runs below to say, not for this change.

## In the game (the integrated Radeon and the RTX 5080)

Chain `amdfix`, run 2026-10-30 (`C:\mcvmdfix\live`): guard on against off (`--d3d12_msaa_line_guard=false`) in the same build, on the integrated Radeon: 1x, fixed dt 60, uncapped, the 31 s drive. Episodes = clusters of detector events of at least 100 blocks.

| Setting | Guard off, episodes a run | Guard on | Frame time, median (off / on) |
|---|---|---|---|
| I1: idle wait on, `--d3d12_submit_on_primary_buffer_end=false --d3d12_threaded_submission_split_draws=100` | 4, 3, 2, 2 | 0, 0, 0, 0 | 39.4 / 39.2 ms |
| O: idle wait off | 3, 3, 5, 1 | 0, 0, 0, 0 | 38.8 / 38.6 ms |
| D: defaults | 0, 1, 0 | 0, 1, 0 | 39.0 / 38.8 ms |

I1 and O are separated (p < 0.001). The two events at the defaults are game content (a can thrown at the camera, the car and a dealer sign), checked by eye; the on sheets of I1 run 1 were checked by eye too, the others are detector counts. So with the guard on the integrated Radeon is as clean with the idle wait off as with it on: the idle wait was hiding the same effect and could be retired in a later release; it is left alone here.

RTX 5080, D3D12, defaults, native, 3 runs a side: mean frame 4.187 ms off, 4.230 ms on (+1.0%), within the spread of the runs (0.06 to 0.08 ms). The picture pairs say nothing: two runs with the guard on differ as much as off against on, because time of day and traffic differ between runs. That no covered sample changes on the RTX 5080 and WARP rests on the program.
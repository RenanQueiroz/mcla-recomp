# The smear over a car's plate on Vulkan (fork issue #11 part B), 2026-10-28

What it was, how it was found, the fix (`patches/rexglue-vulkan-odepth-scale.patch`, option `vulkan_odepth_float24_scale`), and what the checks showed. Every
number says what measured it; what is inferred is marked. The chain `vkblur2` ran on 2026-10-28 (section 4).

## 1. The cause in one paragraph

The game draws the car's four wheels into the stencil in render phase 0x800000 (Vulkan frame log of the creeping orbit at the 3/4-rear angle: draws 3290 to 3293;
D3D12: 3303 to 3306), one draw each, vertex shader `9C638EB844FAA4A1`, pixel shader `7997FE6E65B4803F`, 234 vertices, no colour write, depth test greater-or-equal,
stencil replace with the references 8, 12, 16 and 20 (the vehicle IDs the post phase's motion blur reads). That pixel shader writes depth itself (`oDepth`): of the
3,420 draws of that frame, these four are the only ones that do (also in the 3,472-draw frame at the previous angle and the 3,666-draw frame with the camera at rest).
On a float24 depth buffer the host holds the guest depth halved (`GetHostViewportInfo`: `full_float24_in_0_to_1`, the Vulkan frame log has the wheel draws'
host depth range as [0, 0.5], the other draws of the scene as [0.5, 0] and the shadow phases' as [0, 1]); a pixel shader's depth output is not touched by the viewport, so the shader has to scale it:
the D3D12 translator does (`DxbcShaderTranslator::CompletePixelShader_DSV_DepthTo24Bit`, when the depth format is float24), the Vulkan one did it only inside the
conversion in the pixel shader (`SpirvShaderTranslator::CompleteFragmentShaderInMain`, `kFloat24Truncating/Rounding`). In the default Vulkan mode the wheels' depth
was twice what the buffer holds for the same distance, the test against the car's body passed where it should fail, the wheels' IDs were written over the plate and the
rear lights, and the motion blur of the post phase blurred them along the wheel's velocity: the horizontal smear. (Inferred from the code, the logs and the pictures
below; the draw was then isolated by a conversion for its pixel shader's hash alone, and the fixed build was seen: section 4.)

## 2. What the earlier batches showed (the `vkblur` work, dev layer in the SDK of a scratch tree: a per-draw frame log with a render-phase label, `tmp_vb_skip`, `tmp_vb_convert`)

Pictures from `C:\mcv\vkblur\dev\shots`, read by eye (rows = configurations in one game run, columns = four angles of the camera's orbit). The box-gradient numbers of the
first look are not usable: the car and the camera drift between the configurations of a run, and the first and last `base` rows of every run disagree (4.3 and 2.0).

| Batch (what was changed, Vulkan, RTX 5080 unless said) | Plate |
|---|---|
| E1a: conversion limited to the UI phase, tone map + UI, post, composite, vehicles | smeared in all (only `all` is sharp) |
| E1b: conversion limited to the pre-pass, the world (0xE0000), the soft particles | smeared |
| E1b: conversion limited to the phases 0x1F00000 (translucent, the wheels' 0x800000 among them) | **sharp** at all four angles |
| E1b: conversion limited to the early phases 0x7FF (shadows and the like) | smeared at the last angle; the other angles are in another framing of the car in that row (inconclusive), and the picture is darker overall |
| E1c: the draws of the phases 0x1F00000 left out | **sharp** at all four angles |
| E1c: the UI, the composite left out | smeared at the last angle (the first three are in another framing of the car in the UI row); no help |
| E1c: the post phase left out | garbage: the tone map reads what the post phase makes (not usable) |
| E1e: `tmp_vb_same_enc` (the ownership/transfer bookkeeping of the converting path without the conversion), alone, with the UI | smeared: the cause is not the bookkeeping |
| E1r: the Radeon iGPU, base against `all` | smeared / sharp: not an NVIDIA-only problem |
| E1d: D3D12 | sharp |

The E2 batches (conversion or leaving out for one pixel shader of the UI, post, tone map + composite, vehicle and world phases) never covered the phases 0x100000 to 0x1000000,
and the rows that gave a low gradient number in the first look (the first row of each) are a different framing of the car, not a smear. The frame logs of both APIs
(`C:\mcv\vkblur\fl\vk-u07_1.txt`, `dx-u07_1.txt`) were compared draw by draw: 3,367 of the 3,420 and 3,433 draws match in order by shaders and vertex count, their state registers are
identical but for two draws (a texture size, a stencil reference) and the texture addresses; the 53 and 66 that do not match are in the early (shadow) phases, other content of another frame. The state of
the wheel draws is the same on both APIs. What differs is the host depth.

## 3. The fix

`patches/rexglue-vulkan-odepth-scale.patch`: in `CompleteFragmentShaderInMain`, for a pixel shader that writes depth, not converting, on host render targets:
`gl_FragDepth = oDepth * (float24 depth format ? 0.5 : 1.0)` (the flag is the D3D12 translator's too). `vulkan_odepth_float24_scale` (default true, restart-only)
turns it off. Cost: one multiplication and a select in the shaders that write depth (four draws in a 3,420-draw frame): not measurable, section 4.
CI (llvm-mingw, run 40647579398 of the first commit of the branch `fix/vulkan-plate-blur`): the patch applies, the SDK builds with it and the host code compiles against it, all three jobs green.

## 4. The chain that checks it (`SP\vkblur2\chain.ps1`, one exclusive hold, 22 minutes; run 2026-10-28 03:57 to 04:19) and what it showed

Every run steps the game at a fixed 60 Hz (`fixed_dt` 60, uncapped) and the pad's inputs are counted in frames, so two runs put the car and the camera in the same place and
differ only by what is under test; the picture runs wait for every pipeline (`--async_pipeline_end_of_frame_wait=true --async_pipeline_wait_budget_ms=0 --vulkan_async_skip_incomplete_frames_max_ms=0 --vulkan_async_placeholder_pipelines=true`) so that a frame is complete whatever the shader cache holds, and the cost runs keep the shipped behaviour after a warm-up run. The pad (`orbit_pad` in `SP\vkblur2\tools\mk.py`): boot, free roam, dusk poked into the game's clock, `trigger rt 0.5` (the car creeps), 90
frames, then twice: the camera back at the car's rear (`stick rx 0.5`, 70 frames), the right stick 0.525 to 0.70 in steps of 0.025 every 8 frames with a picture at the steps
5 to 8 (`r0-u05`..`r1-u08`).

| Batch | Runs | Shows |
|---|---|---|
| P | Vulkan fix, off, off, fix (A-B-B-A), Vulkan with `depth_float24_convert_in_pixel_shader=true`, D3D12 | the plate sharp with the fix; how much repeated runs differ (the noise); the fix against the full conversion |
| X | the scratch tree's dev exe (`tmp_vb_convert`, `tmp_vb_skip`), one run: base, the conversion for the wheels' pixel shader `7997FE6E65B4803F` alone, base, that shader's draws left out, base, the conversion for the phase 0x800000 | whether that one shader is the one (rows are one run: read the sheet by eye) |
| S | the same orbit with the car standing: off, fix | whether the smear needs the wheels turning (a steadier repro if it shows) |
| CR | RTX 5080, Vulkan 3x, the drive-spawn timedemo: warm, off, fix, fix, off, full conversion | cost (median frame, p99) |
| RP | the orbit on the Radeon iGPU (`--vulkan_device=1`): off, fix | the fix on AMD |
| CA | Radeon 1x: warm, off, fix, fix, off, full conversion | cost on a weak GPU |
| W | the city drive (picture every 60 frames: fix, off, fix) and the race "Sunset and Vine" (every 450 frames: fix, off) | pictures elsewhere unchanged |

Results: `SP\vkblur2\results\summary.txt` of that session (`python -P -E SP\vkblur2\tools\analyze.py` writes it and the sheets `orbit-P.jpg`, `orbit-S.jpg`, `orbit-RP.jpg`, `cfg-X.jpg`). All 28 runs finished
(`script done: ok`, 0 errors counted). The build was the fix branch alone (`fast-setup`, the SDK rebuilt with the patch).

- **P (RTX 5080, the orbit; the sheet `orbit-P.jpg`, rows = runs, columns = the 8 pictures of two rounds):** with the fix (two runs), with the full conversion and on D3D12 the plate's text is readable in all 8 pictures; with the option off (two
  runs) it is cut off or faded in most pictures from the third of a round on. Read by eye. The gradient number of a box on the plate (HIGH = sharp, means of 8: fix 3.49 and 3.39, off 3.17 and 2.48, conversion 4.20, D3D12 3.65) does
  not separate the runs well (the car and the camera drift between runs, and the first picture of a round is sharp in every run), so it is not the evidence; the sheets are. Two runs with the option off differ from each other
  (crop difference 14.1) more than the fix differs from the option off (5.5): the smear is not the same in two runs.
- **S (the car standing):** the plate is sharp with and without the fix (gradient 2.31 and 2.32).
- **RP (Radeon iGPU):** readable in 8 of 8 with the fix, cut off or faded in 4 of 8 with it off (the last two pictures of each round).
- **X (a development build, one run, rows = configurations):** the conversion applied to the wheels' pixel shader `7997FE6E65B4803F` alone: the plate is readable at all four angles (the rows around it, `base`, `base2` and `base3`,
  show the smear at the last two angles); leaving that shader's draws out: readable at all four; the conversion limited to the render phase `0x800000`: readable, slightly faded at the last angle. So the wheels' shader is the one.
- **CR (RTX 5080, Vulkan 3x, the drive; median frame, two runs per arm after a warm-up run):** off 7.781 and 7.742 ms (mean 7.761), fix 7.919 and 7.687 ms (mean 7.803): +0.53%; the full conversion 9.416 ms (+21.3%).
  **CA (Radeon iGPU, Vulkan 1x):** off 33.021 and 32.870 ms (mean 32.945), fix 33.101 and 33.354 ms (mean 33.227): +0.86%; the full conversion 44.684 ms (+35.6%). Two runs of one arm differ by 0.5% and 3.0% on the RTX 5080
  (off, fix) and by 0.5% and 1.0% on the Radeon: the fix's cost is inside the noise.
- **W (the city drive, 7 pictures a run; the race, 5):** whole-frame mean absolute difference, fix against off: 7.0 to 29.5 (city), 18.0 to 38.6 (race); fix against fix: 12.8 to 22.5 (city). The runs are not the same scene (traffic), so these
  numbers do not say whether anything else changed; no sheet of them was looked at.

## 5. Not done

- Other scenes: the city and the race pictures of section 4 (W) were not compared by eye and the numbers do not separate the fix from the traffic.
- A GTX 1060 (the reporter's GPU), other drivers, a day or night scene other than dusk, the PS5 and Linux builds (same SDK code, not built or run).
- Other pixel shaders that write depth in scenes that were not logged (a city frame and a race frame would show them: the draw log of `tmp_vb_log` in the scratch tree lists them by `wd=1`).
- Whether the D3D12 translator's scaling is also right for a depth-writing shader on a D24S8 (unorm) buffer: the flag keeps it at 1.0 there, as D3D12 does; no draw of that kind was seen.

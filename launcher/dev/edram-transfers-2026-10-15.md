# EDRAM transfers at 3x (2026-10-15)

Roadmap item 3: the ownership transfers between host render targets, about a
quarter of the GPU frame at 3x in the race and a third on the title screen
(`race-benchmark-2026-10-15.md`, `profiling-2026-10-14.md`). Windows, RTX 5080,
Ryzen 7 9800X3D, launcher settings, 3x (2x MSAA).

## What causes them

`edram-transfer-log.sdk.patch` (D3D12, `TMPXF`) logs every draw that changes
the bound render targets or causes transfers, in every `MCLA_XF_EVERY`-th
frame, to `MCLA_XF_FILE`. Each line gives:
- the draw's state: primitive, index count, depth / stencil, color mask,
  blending, alpha test, scissor, vertex transform, the pixel shader;
- the vertices' positions and first interpolators, run on the CPU;
- the pixel shader's color on the CPU, with the registers at 0 and at 1;
- the transfers: the range of tiles and the source render target.
`xf-classify.py <log>` sorts the transferred samples by what triggered them.

Race at 3x, 11 logged frames, mega-samples transferred a frame (at 1x scale;
nine times as many host samples at 3x):

| Trigger | Per frame | Share |
|---|---|---|
| A clear rectangle binding its target (`clear`) | 8.77 | 49% |
| The copy back from a clear rectangle's target to the same target in another MSAA mode (`back`) | 5.02 | 28% |
| Everything else | 4.26 | 24% |

The clear rectangles are Direct3D 9's clears:
- one rectangle (a 3-vertex rectangle list) in screen space;
- the same pixel shader (`A4A965C189287B99`, the color from an interpolator)
  or none;
- depth "always", written, stencil "always" / "replace" / write mask 0xFF;
- no blending.
Most are drawn with 4x MSAA over targets drawn with 2x or 1x, because a 4x
pixel covers four samples:
- **The main scene** is drawn in two tiles, 1280x512 and 1280x208 at 2x MSAA
  (`D24FS8` at tile 0, `2_10_10_10_FLOAT` with exponent bias 4 at tile 1024).
  Each is cleared by a 640x512 or 640x208 rectangle at 4x MSAA. The whole tile
  went to 4x targets and back for every clear: 1.84 Msamples each way for
  depth and for color, the largest items in the log.
- **The HUD's depth** (1x `D24FS8` at tile 0, pitch 16) is cleared by a
  640x360 rectangle at 4x.
- **Smaller targets** are cleared the same way: reflections and shadows at
  pitch 2, 4, 6, 7 and 13, some in two parts (a strip at 2x, the rest at 4x).
Clear colors seen:
- 0 0 0 0, 0 0 0 1 and 1 1 1 1;
- greys of 31, 51, 102 and 127 / 256.

## The change

`rexglue-clear-rectangle-draws.patch` (both backends) does these draws as host
render target clears, as resolves clear (`patches/README.md` has the
conditions). Each clear goes into the MSAA mode the range was drawn with next
after the same clear the previous time. So from the second frame on:
- the main scene's tiles are cleared in their 2x targets;
- the HUD's depth in its 1x target;
- with no transfers.

Not changed, about a quarter of the samples (the `other` row), for item 4:
- **The final 1x `8_8_8_8` target at tile 720** overlaps the scene's color
  range (tile 1024 on).
- **The 1x and 2x depth** at tile 0 alternate between passes.
- **The post-processing chain** at pitches 4 and 8 reuses tiles 0 to 368 in
  different formats.

## Checking it

The dev pad's `set <option> <value>` (`src/dev_pad.cpp`) changes an option
while the game runs, so on and off can be compared within one run, 3 engine
frames apart. Comparing screenshots from separate runs does not work here:
- the time of day differs from run to run;
- so does the sky's colour;
- traffic and the HUD's song differ too.

Mean absolute difference out of 255 (and pixels off by more than 24, at
quarter size):

| Scene | API | On vs off | On vs on | Off vs off |
|---|---|---|---|---|
| City, standing | D3D12 | 0.27 (0.13%) | 0.32 (0.13%) | 0.14 (0.09%) |
| City, pause menu | D3D12 | 0.27 (0.03%) | 0.28 (0.03%) | 0.20 (0.02%) |
| City, standing | Vulkan | 0.28 (0.22%) | 0.26 (0.27%) | 0.32 (0.26%) |
| City, pause menu | Vulkan | 0.27 (0.02%) | 0.33 (0.04%) | 0.27 (0.03%) |

Driving, the map zoom-out and the race differ between any two shots by
motion. Side by side they show nothing out of place: the rain, the
opponents, the checkpoint flare, the HUD, the minimap and the reflections all
look the same.

## Measurements

GPU timestamps (`gpu-timestamps.sdk.patch` and `gpu-timestamps-vulkan.sdk.patch`,
`MCLA_GT_EVERY=1`), one timedemo run a side.
- **Frames used:** those inside the `race` segment of `race.pad`, and 30 s of
  standing at the spawn point after `boot-to-roam.pad`.
- **Columns:** milliseconds, median frame; `xfer` is transfers plus clears.

| | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| City, standing: GPU frame | 13.73 | 10.21 | 12.72 | 10.54 |
| draws | 5.67 | 5.48 | 5.04 | 5.28 |
| xfer | 3.61 | 0.31 | 2.72 | 0.37 |
| resolves | 2.20 | 2.16 | 1.93 | 1.80 |
| texture loads | 1.58 | 1.62 | 1.80 | 1.85 |
| uploads | 0.68 | 0.64 | 0.57 | 0.49 |
| render pass begins | | | 0.55 | 0.61 |
| Race: GPU frame | 13.15 | 10.44 | 14.16 | 13.11 |
| draws | 6.16 | 6.41 | 6.77 | 7.60 |
| xfer | 3.30 | 0.35 | 2.61 | 0.42 |
| resolves | 1.72 | 1.71 | 1.57 | 1.61 |
| texture loads | 1.25 | 1.26 | 1.54 | 1.50 |
| uploads | 0.71 | 0.68 | 0.74 | 0.80 |
| render pass begins | | | 0.82 | 0.89 |

The race is not repeatable, so its single runs differ in where the car is when
a frame is timed. Vulkan's race frames with the setting on also had more time
in the `draws` column, which takes everything outside the marked work,
including waits. The timedemo below is the better measure there.

Scripted timedemo (`devrun.py`; updates of exactly 1/60 s, uncapped). `drive-spawn.pad` is fullscreen, with five runs per configuration; `race.pad` is windowed, with three. Runs alternate after a warm-up. Median frame time, with the 1% low in brackets:

| Segment | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| Free roam, standing | 13.98 ms (61 FPS) | 10.67 ms, -23.7% (74 FPS) | 12.10 ms (70 FPS) | 9.78 ms, -19.2% (82 FPS) |
| Drive from the spawn point | 13.81 ms (59 FPS) | 10.27 ms, -25.6% (74 FPS) | 12.27 ms (69 FPS) | 9.83 ms, -19.9% (84 FPS) |
| Map zoom-out (loading) | 12.69 ms (22 FPS) | 8.73 ms, -31.2% (26 FPS) | 11.33 ms (29 FPS) | 7.73 ms, -31.8% (30 FPS) |
| Race | 12.92 ms (57 FPS) | 10.54 ms, -18.4% (69 FPS) | 13.62 ms (59 FPS) | 11.86 ms, -12.9% (64 FPS) |

- **Native resolution:** no change on either API (6.82 to 7.01 ms in all three segments, within 1%), as one of the game's own threads holds the frame there.
- **D3D12 at 3x with a 60 FPS cap:** pacing is unchanged, 97.3% of frames within 1 ms of the target on against 97.1% off.

With the setting on, Vulkan still has the shorter frames in the city at 3x (4 to 12%,
9.78 against 10.67 ms standing), but D3D12 now has 11% shorter frames in the
race (10.54 against 11.86 ms), where Vulkan's frame gained less.

Scratch scripts for this (not in the repository): a `bench`-style series with
the option on and off for both APIs, `devrun.py run` with `MCLA_GT_EVERY=1` and
the frames inside a segment kept by file time, and screenshots with `set`
between them.

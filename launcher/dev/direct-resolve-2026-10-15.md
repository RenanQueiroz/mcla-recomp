# Direct resolves at 3x (2026-10-15)

Roadmap item 4b, after `rexglue-texture-load-direct.patch`: every resolve
dumped the host render targets over its area into the EDRAM buffer (every
sample) before the resolve shader converted it, 60% of the resolve time at 3x
(`resolves-2026-10-15.md`). `patches/rexglue-direct-resolve.patch` makes the
SDK's `direct_host_resolve` (on by default, a stub until now) read the render
target that owns the area directly. Windows, RTX 5080, Ryzen 7 9800X3D,
launcher settings.

## The change

Details in `patches/README.md`. In short:
- the resolve copy shaders with every EDRAM buffer load replaced by a load of
  the same sample from the render target, converted to its EDRAM
  representation the way dumping does (`src/graphics/shaders/resolve_direct*`,
  built with the xenia 0b2ffa3 shader sources like the load shaders);
- used when one render target with the resolve's pitch, sample count and
  sample size owns the whole area, the samples read are within the pixels, the
  resolution is scaled, and the copy shader is fast 32bpp, fast 64bpp, full
  8bpp, full 32bpp or full 64bpp (ten shaders a backend, 1.3 MB of headers
  without disassembly); otherwise the resolve dumps as before;
- D3D12: a root signature with two SRV tables (float, uint; null SRVs for the
  unused one); Vulkan: a two-image descriptor set (dummy 1x1 images for the
  unused one on colour render targets, the transfer set on depth ones).

What is left on the EDRAM buffer path here: the race's 128x128 4x resolves
into `k_1_5_5_5` (full 16bpp, 0.07 ms a frame, about ten a frame) and one
320x184 resolve standing in the city (6 us) whose area isn't owned by one
render target with its pitch.

## Bit-exact validation

`direct-resolve-validate.sdk.patch` (`TMPDV`, on top of the patch), setting
`direct_resolve_validate`: one direct resolve at a time (the k-th of a frame,
k advancing so every resolve of a frame comes round) also goes through the
EDRAM buffer into the same destination, with both results read back and
compared when the submission completes; differences are logged with the first
16 bytes, and every 50 a summary per kind (copy shader, MSAA, render target
format, resolve format, destination format, samples, conversion, exponent
bias, swap, endian). Real time at 3x, `validate-city` (40 s at the title for
the attract movie, the map zoom-out, a minute standing in the city by day, a
drive) and `race.pad` (rain, opponents):

| Run | Resolves compared | Differing |
|---|---|---|
| D3D12, city | 7,300 | 259, all 4x MSAA depth |
| D3D12, race | 4,300 | 68, all 4x MSAA depth |
| Vulkan, city | 14,000 | 1,059, all 4x MSAA depth |
| Vulkan, race | 6,200 | 142, all 4x MSAA depth |

Every colour kind (fast 32bpp 1x 8_8_8_8, gamma, 32_FLOAT; fast 64bpp
16_16_16_16_FLOAT and 32_32_FLOAT; full 32bpp and 64bpp from 7e3 2x and 4x
averaged, through the 64-bit view, with exponent bias; full 8bpp from 4x
8_8_8_8 averaged) and 1x and 2x depth matched every time.

**The 4x MSAA depth differences are a bug of the EDRAM buffer path.** A
depth value off by one, or the far-plane value where the other has depth, in a few
thousand texels a resolve: a neighbouring sample. `resolve_fast_32bpp_4xmsaa`
loads 16 consecutive values from the first pixel's sample for 8 pixels, but
depth tiles have their halves swapped in the EDRAM layout (40 samples times the
resolution scale each). When a thread's 16 samples cross the middle of a tile
(half a tile is 120 samples at 3x, 40 at 1x, neither a multiple of 16), those
after the middle come from where the next sample row is: the pixel's other
vertical sample, or, from a tile's last row, the next tile. 2x depth (8
samples a thread) never crosses. The direct shader reads each sample where it
is. To confirm, the direct fast 32bpp shader was built once with those reads
copied in (4x depth only, the next row or the next tile past the middle):

| Run, with the old reads copied in | Resolves compared | Differing |
|---|---|---|
| D3D12, city | 7,800 | 0 |
| D3D12, race (including 4x 20e4 depth) | 4,750 | 0 |
| Vulkan, city | 11,000 | 0 |

## GPU timestamps

The two `gpu-timestamps` patches and `resolve-split-timestamps.sdk.patch`
(refreshed for this patch: the direct resolve shader is its own step,
`rdirect`), one timedemo run a side, `MCLA_GT_EVERY=1`, frames inside the
segment, median per frame at 3x:

| | Resolves, off | Resolves, on | GPU frame, off | GPU frame, on |
|---|---|---|---|---|
| D3D12, free roam (day) | 2.23 ms (dump 1.36, copy 0.77) | 1.07 ms (direct 0.98) | 10.53 ms | 9.41 ms (-10.6%) |
| Vulkan, free roam (day) | 1.90 ms (dump 1.09, copy 0.65) | 1.12 ms (direct 1.02) | 10.16 ms | 9.23 ms (-9.2%) |
| D3D12, race | 1.78 ms (dump 1.03, copy 0.64) | 0.97 ms (direct 0.77) | 10.64 ms | 9.98 ms |
| Vulkan, race | 1.65 ms (dump 0.92, copy 0.54) | 1.23 ms (direct 0.86) | 12.66 ms | 11.98 ms |

(The race's GPU frame varies between runs with the opponents' positions; the
resolve times are what to compare there.)

Per resolve, D3D12, free roam:

| Resolve | Off (dump + copy) | On |
|---|---|---|
| Scene colour 1280x512 2x 7e3, samples 0+1, to 8_8_8_8 | 206 us (105 + 95, clear 6) | 104 us (98, clear 6) |
| Scene colour through the 64-bit view, to 16_16_16_16_FLOAT | 157 us (91 + 65) | 92 us |
| Depth quadrant 640x640 4x (daylight), sample 0 (x4) | 116-123 us (90-95 + 25-27) | 33-49 us |
| Scene depth 1280x512 2x 20e4 (x2) | 118 us (95 + 22) | 40 us |

## Benchmark

`devrun.py` runs on the save copy, `--direct_host_resolve=false` for off, the
configurations alternating after one warm-up round each. `drive-spawn.pad`
fullscreen (the map zoom-out, 10 s standing at the spawn point, the drive);
`race.pad` windowed. Timedemo: `--mcla_fixed_dt=60`, uncapped. Median frame,
spread of the run medians, 1% low, the worst frame (median of the runs' worst):

Timedemo at 3x, five runs a configuration:

| Segment | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| Free roam | 10.09 ms (2.2%), 81 FPS, 13.5 ms | 8.99 ms (1.5%), 91 FPS, 12.1 ms | 9.33 ms (1.6%), 90 FPS, 11.4 ms | 8.46 ms (1.1%), 99 FPS, 12.7 ms |
| Drive | 9.79 ms (2.3%), 79 FPS, 13.3 ms | 8.63 ms (0.7%), 87 FPS, 12.2 ms | 9.39 ms (1.2%), 88 FPS, 11.6 ms | 8.67 ms (2.9%), 95 FPS, 10.8 ms |
| Map zoom-out | 9.04 ms (20.9%), 26 FPS, 47.9 ms | 7.23 ms (12.0%), 31 FPS, 40.0 ms | 8.37 ms (17.6%), 31 FPS, 43.4 ms | 7.34 ms (8.2%), 32 FPS, 41.9 ms |

Real time at 3x, three runs:

| Segment | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| Free roam | 10.01 ms (2.5%), 84 FPS, 12.7 ms | 8.81 ms (2.7%), 90 FPS, 11.7 ms | 9.22 ms (0.6%), 96 FPS, 10.6 ms | 8.57 ms (1.9%), 102 FPS, 10.2 ms |
| Drive | 9.12 ms (1.6%), 89 FPS, 12.0 ms | 8.04 ms (1.1%), 101 FPS, 10.4 ms | 8.90 ms (1.7%), 96 FPS, 10.6 ms | 8.22 ms (1.2%), 103 FPS, 10.1 ms |
| Map zoom-out | 8.01 ms (13.5%), 38 FPS, 43.1 ms | 7.73 ms (10.3%), 40 FPS, 43.5 ms | 7.54 ms (15.3%), 43 FPS, 39.2 ms | 7.33 ms (4.0%), 49 FPS, 42.4 ms |

Race, timedemo at 3x, windowed, three runs:

| | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| Race | 10.18 ms (3.5%), 71 FPS, 16.8 ms | 9.61 ms (1.7%), 75 FPS, 16.6 ms | 11.60 ms (1.5%), 65 FPS, 19.5 ms | 11.07 ms (3.3%), 67 FPS, 23.6 ms |

Frame caps at 3x, two runs, free roam and the drive: frames within 1 ms of
the target.

| Cap | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| 60 FPS | 97.3% | 97.4% | 98.6% | 97.8% |
| 120 FPS | 26.0% (median 10.09 ms) | 92.7% (8.84 ms) | 83.5% (8.93 ms) | 97.2% (8.39 ms) |

Native, timedemo, three runs: the same on and off (direct resolves are used
with resolution scaling only), 6.81 to 6.92 ms in every segment on both APIs,
spread 0.3 to 1.9%.

The map zoom-out is mostly loading; its medians move with where the loads
fall (spread up to 21%).

## Checks

- **Runs:** every run of this step (the 96 benchmark runs, the validation,
  timestamp, screenshot and debug layer runs) exited 0 with its script done
  and the real save unchanged; no silent exit. No frame over 250 ms in the
  benchmark; the worst frame of an uncapped Vulkan drive was 10.9 ms on and
  11.9 ms off (no ~280 ms frame).
- **Screenshots** with the setting switched on and off within one run (the
  dev pad's `set`), 3 engine frames apart, both APIs at 3x: the attract movie,
  the map zoom-out, standing in the city, a drive and the race (rain,
  opponents, HUD) look the same either way. In the pause menu standing in the
  city, eight shots alternating, 6 frames apart: the pixels that differ between
  on and off and stay the same from shot to shot are 0.08% of the picture on
  D3D12 and 0.11% on Vulkan (6,484 and 9,497), 1 to 169 levels out of 255,
  mostly columns 4 pixels wide 120 pixels apart (the middles of 4x tiles at 3x,
  where the old path misread), the rest within 4 levels on a parked car. With
  the old reads copied into the direct shader (the test build above), on and
  off were identical at every still pixel on both APIs. (Columns on screen
  mean the four 640x640 4x depth quadrants, one 1280x1280 texture by their
  destinations, map 1:1 onto the screen: a screen depth used in daylight
  rather than the shadow map `resolves-2026-10-15.md` took them for.)
- **D3D12 debug layer** (`--d3d12_debug=true`, output through the debugger
  stream) through `drive-spawn.pad` and `race.pad`: no errors or corruption
  messages; the only warnings are unrelated to resolves (initial states
  ignored for buffers, blend factors with MIN/MAX, live objects at exit).
- **Vulkan validation** with synchronization validation
  (`VK_LAYER_VALIDATE_SYNC=1`, `VK_LAYER_SYNCVAL_SHADER_ACCESSES_HEURISTIC=1`)
  through the same two: `vkCmdEndQuery` 07007 and 01923,
  `ShaderOutputNotConsumed`, and 10 write after write hazards between texture
  load dispatches into their scratch buffer; the same with
  `--direct_host_resolve=false`. Nothing from the direct resolves.
  (Correction, round 3: the race run never got to the race, its menus out of
  step at the validation's frame rate, and the layer stops after 10 reports of
  a message; both redone below.)

## Round 3: review fixes, full 16bpp, native and 2x, the EDRAM path's 4x fix

After an independent review of the patch and with the scope widened:
- **Vulkan dummy barriers:** the first direct resolve transitioned all four
  dummy images, some of which can be null (no multisampled integer sampled
  images, which Vulkan only guarantees single-sampled, or a failed creation).
  Only the existing ones are transitioned now.
- **Multisampled support per kind on Vulkan:** the pipelines no longer need
  both multisampled dummies. A multisampled colour source needs multisampled
  integer sampled images either for itself (the 16- and 32-bit formats) or for
  the dummy of its unused integer binding (8_8_8_8 and the others read as
  float), so without them multisampled colour sources go through the EDRAM
  buffer; multisampled depth (the stencil view is a depth/stencil sampled
  image) and single-sampled sources still resolve directly.
- **No search without a shader:** the resolve's sample count picks the direct
  pipeline first, and the render target ownership walk runs only when there's
  one.
- **The sample selection check** is gone: `GetResolveInfo` already sanitizes
  it (`SanitizeCopySampleSelect`).
- **Full 16bpp** (the race's 128x128 4x resolves into `k_1_5_5_5`) and the
  **unscaled variants** of all six copy shaders: 24 shaders a backend, 3.1 MB
  of compact headers (1.3 MB before); `rexglue-direct-resolve.patch` is
  3.37 MB (1.46 MB before), the fix 265 KB.
- **`rexglue-resolve-4xmsaa-depth-samples.patch`**, a patch of its own before
  this one, fixes the 4x depth misreads in the EDRAM path's
  `resolve_fast_32bpp_4xmsaa` (pixels 4 to 7 of a thread get their own
  address), the SDK's two headers of it rebuilt for each API with the
  toolchain that rebuilds the current ones byte for byte. Fallback resolves no
  longer differ from direct ones, so the validation build compares against the
  fixed path and expects no difference at all.

### Validation

`direct-resolve-validate.sdk.patch` now also covers unscaled resolves (the
destination read back from shared memory). Real time, `validate-city` and
`race.pad`, every kind with zero differing bytes:

| Resolves compared | D3D12 city | D3D12 race | Vulkan city | Vulkan race |
|---|---|---|---|---|
| Native | 23,850 | 11,450 | 22,700 | 11,100 |
| 2x | 18,900 | 10,100 | 23,000 | 10,900 |
| 3x | 7,000 | 5,100 | 11,100 | 7,350 |

162,550 in all, 9,942 of them 4x depth (`kD24S8` and `kD24FS8`) and 6,584
full 16bpp (4x `k_8_8_8_8` averaged into `k_1_5_5_5`).

### GPU timestamps

Same stack as before plus `texload-split-timestamps.sdk.patch`, one timedemo
run a side, median per frame:

| Resolves | D3D12 off | D3D12 on | Vulkan off | Vulkan on |
|---|---|---|---|---|
| Native, free roam | 0.48 ms (dump 0.22, copy 0.20) | 0.33 ms (direct 0.28) | 0.50 ms (dump 0.21, copy 0.16) | 0.42 ms (direct 0.32) |
| Native, race | 0.54 ms (dump 0.21, copy 0.25) | 0.38 ms (direct 0.31) | 0.55 ms (dump 0.21, copy 0.19) | 0.48 ms (direct 0.35) |
| 2x, free roam | 1.09 ms (dump 0.58, copy 0.43) | 0.67 ms (direct 0.59) | 0.93 ms (dump 0.49, copy 0.31) | 0.65 ms (direct 0.55) |
| 2x, race | 1.00 ms (dump 0.49, copy 0.40) | 0.66 ms (direct 0.58) | 0.88 ms (dump 0.41, copy 0.29) | 0.70 ms (direct 0.56) |
| 3x, free roam | 2.23 ms (dump 1.36, copy 0.78) | 1.08 ms (direct 0.98) | 1.90 ms (dump 1.09, copy 0.67) | 1.09 ms (direct 0.98) |
| 3x, race | 1.79 ms (dump 1.04, copy 0.64) | 0.93 ms (direct 0.82) | 1.65 ms (dump 0.82, copy 0.54) | 1.10 ms (direct 0.96) |

Full 16bpp took the race's last dump away: 3x race 0.97 to 0.93 ms on D3D12
and 1.23 to 1.10 ms on Vulkan against the first round. At native and 2x the
frame span (first to last timestamp) here is set by the game thread (7 to 11
ms), so it isn't the GPU's work; what a GPU-bound system gains is the resolve
time saved: 0.15 and 0.08 ms a frame at native standing in the city (D3D12,
Vulkan), 0.42 and 0.28 ms at 2x.

The fixed EDRAM path's 640x640 4x depth copies at 3x: 25 to 26 us on D3D12
(25 to 27 before), 29 to 30 us on Vulkan (26 to 27 before), so a few us at
most.

### Texture loads at native (texture_load_direct)

Not extended: at native, the textures reloaded every frame here are the
resolved 1280x720 `8_8_8_8` (three times), the 1280x720 depth (twice), the
1280x1280 depth and a 1280x720 `16_16_16_16_FLOAT`. Their copies from the
scratch buffer, the only pass a direct load removes, take 0.04 ms a frame on
D3D12 and 0.06 ms on Vulkan standing in the city (all texture loads 0.28 and
0.41 ms). Loading them directly would take unscaled variants of the four load
shaders for both APIs and either typeless textures with unordered access for
every unscaled single-level 2D texture of those formats (static ones too,
whose sampling could lose compression, on AMD especially) or tracking of
resolved memory at 1x, which the texture cache doesn't do. For at most 0.06
ms a frame here (about 0.2 ms on a GPU a quarter as fast), it stays
resolution-scaled only.

### Benchmark

`devrun.py`, alternating after one warm-up round each, timedemo
(`--mcla_fixed_dt=60`, uncapped), `drive-spawn.pad` fullscreen and `race.pad`
windowed. Median frame (spread of the run medians), 1% low:

| Segment | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| 3x free roam (3 runs) | 10.35 ms (1.8%), 75 FPS | 9.19 ms (2.3%), 85 FPS | 9.76 ms (3.2%), 76 FPS | 8.96 ms (1.8%), 86 FPS |
| 3x drive | 10.09 ms (1.9%), 74 FPS | 8.88 ms (0.5%), 79 FPS | 9.94 ms (0.7%), 69 FPS | 9.03 ms (1.7%), 73 FPS |
| 3x map zoom-out | 8.53 ms (10.7%), 27 FPS | 7.38 ms (3.2%), 31 FPS | 8.20 ms (15.6%), 29 FPS | 7.88 ms (11.4%), 30 FPS |
| 3x race (3 runs) | 10.17 ms (2.5%), 71 FPS | 9.29 ms (2.5%), 75 FPS | 11.29 ms (1.0%), 66 FPS | 10.79 ms (1.3%), 68 FPS |
| 2x free roam (5 runs) | 7.45 ms (5.2%), 101 FPS | 7.01 ms (3.2%), 107 FPS | 6.89 ms (1.2%), 107 FPS | 6.84 ms (0.9%), 111 FPS |
| 2x drive | 7.21 ms (1.4%), 92 FPS | 7.12 ms (2.1%), 102 FPS | 7.14 ms (3.8%), 88 FPS | 7.05 ms (4.6%), 93 FPS |
| 2x map zoom-out | 7.00 ms (2.4%), 31 FPS | 6.92 ms (1.7%), 33 FPS | 7.03 ms (2.6%), 31 FPS | 6.96 ms (2.7%), 33 FPS |
| 2x race (3 runs) | 8.10 ms (3.5%), 85 FPS | 7.86 ms (7.8%), 84 FPS | 8.93 ms (4.8%), 66 FPS | 8.29 ms (7.1%), 67 FPS |
| Native free roam (3 runs) | 6.86 ms (0.4%), 113 FPS | 6.79 ms (1.5%), 113 FPS | 6.81 ms (0.3%), 112 FPS | 6.84 ms (0.8%), 111 FPS |
| Native drive | 6.83 ms (0.8%), 112 FPS | 6.84 ms (1.5%), 113 FPS | 6.87 ms (0.3%), 101 FPS | 6.90 ms (0.5%), 103 FPS |

- At 3x the same gains as the first round (D3D12 -11 and -12%, Vulkan -8 and
  -9%), with free roam and the drive 2 to 6% slower on and off alike than in
  the first round's runs; the D3D12 race gains more with full 16bpp (-8.7%
  against -5.6%), the Vulkan race the same (-4.4% against -4.6%).
- At 2x the frame is close to the game thread's limit (native 6.8 to 6.9 ms),
  so it gains less than the resolve time: -5.9% standing on D3D12, -0.7% on
  Vulkan, -1.2 and -1.3% driving, -3.0 and -7.2% in the race; the 1% lows
  rise more (D3D12 drive 92 to 102 FPS).
- With a 120 FPS cap at 2x (2 runs): 98.5% of frames within 1 ms of the
  target off and 98.6% on (D3D12), 99.4 and 99.5% (Vulkan).
- Native: unchanged, the game thread's frame (6.79 to 6.97 ms in every
  segment, on and off).
- 100 runs, all exited 0 with the script done; no frame over 250 ms.

### Checks

- **Screenshots**, settled pause menu standing in the city, eight shots
  alternating on and off (`set`), at native and 3x on both APIs: identical at
  every pixel that stays the same from shot to shot (921,000 of 921,600 at
  native, 8,292,000 of 8,294,400 at 3x; the rest is the menu's animation).
  The first round's 0.08 to 0.11% of differing pixels were the EDRAM path's
  4x depth misreads, now fixed.
- **D3D12 debug layer** (`--d3d12_debug=true`) through `drive-spawn.pad` and
  `race.pad` at native and 3x: no errors or corruption messages; the warnings
  are the same unrelated ones as before (initial states ignored for buffers,
  blend factors with MIN/MAX, live objects at exit).
- **Vulkan validation with synchronization validation** through the city and
  the race (a short race pad: the race's start and 900 updates of it) at native
  and 3x. The layer reports a message only 10 times by default, which would
  hide later hazards of the same kind, so these ran with
  `VK_LAYER_ENABLE_MESSAGE_LIMIT=0`: thousands of `ShaderOutputNotConsumed`,
  and 268 to 1,660 write after write hazards a run, all between compute
  dispatches into either the texture loads' scratch buffer or the EDRAM buffer
  (dumps and other stores to it), the same two kinds as in a run with
  `--direct_host_resolve=false`. None on a render target image, a resolve
  destination (set 1) or from a direct resolve pipeline. With the limit on,
  also `vkCmdEndQuery` 07007 and 01923 as before.
- **The race under synchronization validation:** the menus run at 10 to 20 FPS
  there, and in the first runs (the first round's and this round's) the waits
  in time alone let the menu transitions miss presses: the race never
  started, the script's `wait_until roam` timed out, and the script carried on
  in the menu until the end (the first round's "through the race" covered
  only the menus). `boot-to-race.pad` now waits at least as many updates as at
  60 FPS too (`wait 300ms 18f`, until both have passed; no change above 60
  FPS), and a failed line ends the script and the run
  (`devrun.py` reports `SCRIPT FAILED`). With them the race starts in both
  validation runs. No other run of either round had a failed line.
- **Watch list:** no silent exit, and no frame near 280 ms in a Vulkan drive
  (the worst uncapped one 22 ms, in the second 3x series, on and off alike).

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

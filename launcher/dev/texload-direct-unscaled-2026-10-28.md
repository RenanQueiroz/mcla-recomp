# Texture loads without the copy at native resolution (2026-10-28)

`patches/rexglue-texture-load-direct-unscaled.patch`, setting `texture_load_direct_unscaled` (default on). Everything in this note is marked as measured or not. The exactness check is still to do (see Results).

## Why

At native resolution a texture load decodes the guest's data from the shared memory into a scratch buffer with a compute shader and copies the buffer into the texture
(`CopyTextureRegion` on D3D12, `vkCmdCopyBufferToImage` on Vulkan). `texture_load_direct` already decodes straight into the texture, in one pass, but only with
resolution scaling. `gpu-weak-2026-10-27.md` put the loads at 3.8 ms of a 38.9 ms GPU frame on the integrated Radeon (D3D12, native, free roam), 2.2 ms of it the copy.

Measured (GPU timestamp frames of the shadow-probe runs on the Radeon with D3D12 and Vulkan and on the RTX 5080 with D3D12, 2026-10-28, code without this patch, the windows in which the
probe only labels; the brackets of `texload-split-timestamps.sdk.patch`, read as `launcher/dev/texload-split.py` reads them):

| Per frame, native resolution, free roam | Loads | Copy | Dispatch | Barriers | GPU frame |
|---|---|---|---|---|---|
| Radeon, D3D12 (96 frames) | 3.92 ms (56 loads) | 2.20 ms | 1.66 ms | 0.04 ms | 36.3 ms |
| Radeon, Vulkan (622 frames) | 3.36 ms (51 loads) | 1.96 ms | 1.36 ms | 0.01 ms | 33.4 ms |
| RTX 5080, D3D12 (108 frames) | 0.24 ms (48 loads) | 0.12 ms | 0.10 ms | 0.00 ms | 6.55 ms |

The loads that cost the time are all 32 or 64 bits a texel or depth, which the direct shaders cover (Radeon, D3D12, per frame, microseconds of the load, of them the copy):
1280x720 `8_8_8_8` three times 1157 (688); the 1280x1280 `24_8` shadow atlas 738 (404); 1280x720 `16_16_16_16_FLOAT` 618 (321); 1280x720 `24_8_FLOAT` 374 (229);
640x360 `16_16_16_16_FLOAT` twice 319 (173); 512x256 `16_16_16_16_FLOAT` twice 220 (105); 320x180 `16_16_16_16_FLOAT` four times 168 (88); 256x256 `8_8_8_8` four times 136 (78);
128x128 `8_8_8_8` 13.5 times 55 (47). Mip loads (`levels=1-...`) and the block-compressed static textures are small and stay on the buffer path.
So the most the change can take off is about the copy column: 2.2 ms on the Radeon with D3D12, 2.0 ms with Vulkan, 0.1 ms on the RTX 5080, minus whatever the stores into the
swizzled texture add to the dispatch (not known: this is what the chain measures).

## What the patch does

The same four shaders as `texture_load_direct` (32bpb, 64bpb, 20e4 depth, 24-bit unorm depth), no new shader code and no fxc / glslang run. Their guest addressing is
`XeTextureScaledTiledOffset`; with a resolution scale of 1 it is the unscaled load's (`launcher/dev/check_texload_scale1.py` ports both: 172 cases, 0 different), the source is the
shared memory, and the push constants say a scale of 1. A texture qualifies when it is tiled, 2D, single-level, single-layer, outside the packed tail, has 32 or 64 bits a texel and
lies over memory a resolve has written (a bit per 4 KB page in the texture cache, set by `MarkRangeAsResolved`, never cleared; `resolved_unscaled` in the texture key). Then the
backend takes it if it has the format (D3D12: typeless texture with unordered access; Vulkan: the storage view). Details and the list of what is not covered: patches/README.md.

## How it is measured (SP\texdirect\chain.ps1, one exclusive hold of at most 45 minutes)

1. Exactness: the world frozen (`mcla_freeze`: frames are still drawn, so the resolved textures are loaded again every frame), in roam, pause and drive four shots with the setting on, off,
   on, off in the same process (`set texture_load_direct_unscaled`), compared pixel for pixel (`cmp_shots.py`); Radeon D3D12 1x, Radeon Vulkan 1x, RTX 5080 D3D12 1x.
2. Timing: PresentMon frame time and GPU busy of a frozen roam, setting off - on - on - off in separate runs (the off runs start with `--texture_load_direct_unscaled=false`, so they have no
   unordered-access textures: the cost of the flag is in the result), the on runs also switch the setting within the run; the same three configurations. The Radeon has slow hours
   (1.4 times): only adjacent runs are compared.
3. Per-load GPU timestamps (the timestamp layers and the texload split) with the setting on and off in windows of one run: loads, direct loads, load / copy / dispatch ms per frame.

## Results

Chain `texdirect`, 2026-10-28 (02:40 to 03:10, one exclusive hold; the branch's build alone, the SDK of v0.2.0 plus this patch; windowed, uncapped, native resolution).

**Timing** (free roam with the world running, PresentMon; setting off - on - on - off in separate runs, main = the median frame time of the run; the on runs also switch the setting
inside the run, 15 s segments):

| | off (2 runs) | on (2 runs) | change | pairs of adjacent runs | switched inside the runs |
|---|---|---|---|---|---|
| Radeon iGPU, Direct3D 12, frame | 36.587 ms | 35.068 ms | -1.52 ms (-4.15%) | -1.29, -1.75 ms | -3.41%, -3.99% |
| Radeon iGPU, Direct3D 12, GPU busy | 36.547 ms | 35.026 ms | -1.52 ms (-4.16%) | -1.31, -1.73 ms | -3.52%, -3.97% |
| Radeon iGPU, Vulkan, frame | 34.026 ms | 32.639 ms | -1.39 ms (-4.07%) | -1.08, -1.70 ms | -3.74%, -4.31% |
| Radeon iGPU, Vulkan, GPU busy | 33.992 ms | 32.628 ms | -1.36 ms (-4.01%) | -1.07, -1.66 ms | -3.63%, -4.20% |
| RTX 5080, Direct3D 12, frame | 4.300 ms | 4.34 ms in one run, 6.86 ms in the other (that run also ran at 6.85 ms in its own off segments: a slow state of the machine, so that pair says nothing) | no change | | +0.18%, +3.09% |
| RTX 5080, Direct3D 12, GPU busy | 4.377 ms | 4.428 ms | +0.05 ms (+1.2%) | +0.04, +0.06 ms | +0.115 ms (+2.8%), +0.182 ms (+4.5%) |

At native resolution the RTX 5080's frame is held by a game thread, so the frame time cannot show a change in GPU time; the GPU busy time is slightly longer with the
setting on. The loads themselves got cheaper (0.244 to 0.204 ms a frame: the copy 0.118 to 0.016 ms, the dispatch 0.102 to 0.171 ms), so the extra is somewhere else in the frame; not
separated. One candidate (inferred, not measured) is sampling the textures that were created with unordered access, a cost that the review of the patch had doubted. It matters only where the GPU is the limit and the resolution is native, which this GPU is not. The off runs started with
`--texture_load_direct_unscaled=false`, so the cost of the flag itself (unordered access textures) is in the result.

**Per-load GPU timestamps** (windows of 18 frames, the setting switched between windows; Vulkan on the Radeon, 72 frames a side): loads a frame 3.631 ms off, 2.598 ms on; copy 2.108 to 0.075 ms;
dispatch 1.489 to 2.499 ms; 50 loads a frame, 38 of them direct with the setting on; GPU frame 34.78 ms off, 33.59 ms on. The 1280x720 `8_8_8_8` loads (three a frame) went from 1,102 to 675 us
together, the 1280x1280 `24_8` shadow atlas from 698 to 428 us, the 1280x720 `16_16_16_16_FLOAT` from 574 to 467 us. The 128x128 `8_8_8_8` loads (13.5 a frame) got slower, 50 to 88 us
together. On the RTX 5080 (Direct3D 12, 8 frames a side): loads 0.244 to 0.204 ms.

**Exactness: not settled.** The chain's own check was four pictures of the frozen world (setting on, off, on, off in one process), comparing the pixels that agree between the two pictures of
each setting. Pause menu: Radeon Direct3D 12 and Vulkan, 0 differing pixels among 99.98% stable ones (OK); RTX 5080 Direct3D 12, 2,376 differing among 87.0% stable (FAIL, the first pixel at (137, 1)
with a difference of one level). Free roam and driving: FAIL on all three, with 85 to 92% stable pixels in roam and 3 to 16% in driving (differing
among the stable: 11,145 to 91,921 in roam, 2,343 to 3,570 in driving). On 2026-10-28 the same check, with nothing changed at all, reported differences in 3 of 3 roam views of every run, because the
frozen world still flickers (`d3d12-upload-hoisting-2026-10-28.md`, Results): it cannot say either way. Direct-capable textures were created in all three runs (24 logged), no `cannot set`
lines, no log errors. The check that replaced it (eight pictures a block in a balanced order, sham blocks, a fixed frame time) has not been run on this setting; it is part of the validation of v0.3.0. The Direct3D 12
debug layer and the Vulkan validation layer were not run on the new path.


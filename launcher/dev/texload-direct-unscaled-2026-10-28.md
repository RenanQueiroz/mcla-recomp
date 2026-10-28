# Texture loads without the copy at native resolution (2026-10-28)

`patches/rexglue-texture-load-direct-unscaled.patch`, setting `texture_load_direct_unscaled` (default on). Everything in this note is marked as measured or not.

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

Not measured yet: the patch was compiled by the CI (Host code against the SDK) and the chain was written and checked offline (the pictures' comparison, the tables and the pads on
made-up data and on the existing timestamp frames); no game ran. The results go here, with the table of the baseline above next to them.

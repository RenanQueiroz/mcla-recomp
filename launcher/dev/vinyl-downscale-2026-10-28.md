# The vinyls at 2x and 3x: what the resolve downscale pass reads (2026-10-28)

The vinyl fix of 2026-10-27 (`mcla_vinyl_readback`, a rule for `readback_resolve_rules`, `launcher/dev/vinyls-2026-10-27.md`) was checked at 1x only. With
`resolution_scale` above 1 a resolve is written to the SDK's scaled resolve buffer, and the readback first turns it back into the guest's 1x data with the compute pass
`resolve_downscale` (`src/graphics/shaders/resolve_downscale.cs.hlsl`, `.glsl`). The vinyl texture is 512x512 `8_8_8_8` (4 bytes a pixel, with its mip chain). This note is
about what that pass reads. **State: the cause is read in the shader and checked by arithmetic; the fix (`patches/rexglue-resolve-downscale-unit-layout.patch`, option
`resolve_downscale_unit_layout`) is written and compiled by CI; neither the released v0.2.0 nor the fix has been looked at in a picture at 2x or 3x yet (the chain
`downscale` does that), so it is not known how visible the fault was.**

## What the pass reads (read in the shader; arithmetic)

The pass has one thread per guest pixel of a 32x32 tile (`pixel_index` = the pixel's place in memory order within the tile) and copies the host pixel at the byte offset

    tile_index * tile_size_1x * scale_x * scale_y + pixel_index * pixel_size * scale_x * scale_y      (+ the half-pixel offset, in pixels)

of the scaled data: the pixel's guest byte address times the area of a block, as if the `scale_x * scale_y` host pixels of a guest pixel were one contiguous run. The layout of
the scaled data is `XeTextureScaledTiledOffset` (xenia 0b2ffa3, `gpu/shaders/texture_address.xesli`, whose resolve shaders compile to the SDK's exactly): the host pixels
are kept in sub-units of N x 1 (N = 8 for 1 and 2 bytes a pixel, 4 for 4, 2 for 8, 1 for 16), column-major inside one guest unit of N pixels. Take 4 bytes a pixel at 2x2
(N = 4): guest pixels 0 to 3 of a unit have their top-left host pixels at host x 0, 2, 4, 6 of the unit's first host row, which is sub-unit 0 (host x 0 to 3: pixels 0 and 1)
and sub-unit 2 (host x 4 to 7: pixels 2 and 3), at pixel offsets 0, 2, 8 and 10 of the unit's 16 host pixels. The pass reads pixel offsets 0, 4, 8 and 12: pixel 4 is the
first pixel of sub-unit 1, the second host row of guest pixel 0. So guest pixels 1 and 3 get the pixel below the top-left of their left neighbour: **every second pixel of the
vinyl texture repeats its left neighbour** (horizontally half the resolution, and a different row of the block), and at 3x3 one in four does and the others read a pixel
of their own block that is not the top-left one. The first claim is the rainfix2 agent's derivation (`launcher/dev/scaled-resolve-layout-check.py` on `fix/rain-scaled-2`);
this agent re-derived it by hand from the shader and the layout function, and with `launcher/dev/downscale-layout-check.py`, which runs the shader's own address code.

`python -I launcher/dev/downscale-layout-check.py` (a texture of 64x64 here; the script also does 128x32 and 32x96), guest pixels for which the pass reads the top-left host
pixel of the block (the old expression / the new one):

| bytes a pixel | 1x1 | 2x2 | 3x3 | 2x2 with the half-pixel offset | 3x3 with the half-pixel offset |
|---|---|---|---|---|---|
| 1 | 4096 / 4096 | 1024 / 4096 | 512 / 4096 | 1024 / 4096 | 0 / 4096 |
| 2 | 4096 / 4096 | 1024 / 4096 | 512 / 4096 | 1024 / 4096 | 0 / 4096 |
| 4 (the vinyl texture) | 4096 / 4096 | 2048 / 4096 | 1024 / 4096 | 2048 / 4096 | 0 / 4096 |
| 8 (the rain's resolves) | 4096 / 4096 | 4096 / 4096 | 2048 / 4096 | 4096 / 4096 | 0 / 4096 |

(Each cell: the old pass / the new pass, of 4096 guest pixels. At 3x3 the old pass reads a pixel inside the guest pixel's own block for 3072 of 4096 at 4 bytes and 4096 of 4096 at 8
bytes. The unequal scales 1x2, 2x1 and so on are in the script's output.)

## The fix

`resolve_downscale_unit_layout` (default on): the new function `XeDownscaleSourceOffset` in both shader sources finds the guest pixel's unit, the sub-unit of the host pixel to take
(the top-left one of the block, or with `readback_resolve_half_pixel_offset` the block's centre, as a host offset inside the block) and the pixel inside it. The check script
runs that text for every pixel size and scale and says all guest pixels read the right host pixel. The old expression is the `else` of the same function, so the option off is the
old pass. The shader headers are rebuilt with `launcher/dev/build_downscale_shaders.py`, whose `--check` rebuilds the SDK's two headers of this shader from the unchanged sources
byte for byte (fxc 10.0.26100.0 with `/O3 /all_resources_bound`, Vulkan SDK 1.4.363.0), so the headers differ by the source change only.

## The picture check (chain `downscale`; not run when this was written)

The arcade race *Sunset and Vine*, 7 opponents, Easy, clear, noon (the pad of the vinyl agent: boot, pause menu, Modes, Arcade, the options, START), pictures at the race timer
1.5, 3.0, 4.5, 6.0 and 7.5 s (the same moment in every run: the intro camera follows the race timer), on D3D12 and Vulkan:
- the released v0.2.0 (the exe set of C:\mcv\val\exe\next) at 1x, 2x and 3x, and the build of this branch at 2x and 3x, with `--resolve_downscale_unit_layout=false` and with the default;
- crops of the opponents' liveries (the green car with the white and green livery, the 6, PIRELLI, WORK, HKS, in the pictures at 4.5 s and 7.5 s), side by side;
- the vinyl texture itself: `dump 0xA50A0000 1048576` of the guest memory at the end of the intro (the 512x512 `8_8_8_8` base level the readback wrote), compared between the runs by
  the share of 4-byte words that equal the 1x run's, and by the share of units whose pixels 1 and 3 equal 0 and 2 (the fingerprint of the repeat).
Read the sheets and the numbers; what is not known until then: whether v0.2.0's 2x and 3x liveries were visibly wrong, and whether the fix gives the 1x livery.

# Rain not drawn at resolution scale 2x and 3x (fork issue #17), 2026-10-28

Report: the rain is not drawn above native 720p (`resolution_scale` 2 or 3, Direct3D 12 and Vulkan, day and night); at 1x it is. The reporter's pause-menu
frame-rate report is another note.

**State: the cause is confirmed by a trace run (2026-10-28, D3D12, RTX 5080). The fix, `resolve_unscaled_copy_rules` (an SDK patch) with the game option
`mcla_rain_unscaled_copy`, is written and compiled by CI; it has NOT been run yet (the chain `rainfix2` runs it). Every claim below says whether it was read,
measured or inferred.**

## What the earlier notes got wrong

The draw with the vertex shader `284A04E0` (5,952 indices, a 128x128 DXT1 texture, first in render phase `0x02000000`) is the lightning bolt, not the rain
(read: the image's strings `LightningTexture`, `lightning.dds`, `LightningPulseIntensity`; the weather object's pool of 8 timed entries). The rain is the other
draw: **32,768 vertices, a quad list (8,192 quads), vertex shader `11AAD997`, pixel shader `47213022`**, textures 32x32 and 256x64 (`DXT1`), the last draw before
the scene's resolves at the end of phase `0x04000000`. It is in the frame logs at 1x and at 2x (measured, the earlier runs), so the game does issue it at 2x.

## The mechanism (read from the recompiled code and the SDK; confirmed by the trace, below)

- The rain object keeps 16,384 drops in **four 128x128 `k_32_32_FLOAT` render targets** (the image's strings: `Particle Pos XY`, `Particle Pos ZW`, `Particle Vel XY`,
  `Particle Vel ZW`, `UpdateParticles`, `RenderParticles`). One draw (a quad, four colour targets, pixel shader `20F6F275`) updates them in the first phase of every
  frame and **four resolves** (length 131072, pitch 128, height 128, copy-destination format 37) write them to `0x0561F000`, `0x0563F000`, `0x0565F000`, `0x0567F000`.
- The same memory is **the rain's vertex data**: `sub_822EF0D8` makes four vertex buffer objects over the locked textures, `mcRain::Draw` binds them as streams, and the vertex
  shader fetches them as `FMT_32_32_FLOAT` (stride 2 dwords). The console's render-to-vertex-buffer trick. (The draw's fetch constants `vf92` to `vf95` point at `0x0563F000`,
  `0x0563F000`, `0x0565F000`, `0x0567F000` in the trace: the vertex shader reads three of the four buffers, one of them twice; measured.) After the resolves the game issues a "make
  coherent" for the written range with the action `VC | TC` (the SDK's trace log, measured in an old 2x run); `CommandProcessor::MakeCoherent` does nothing here (read).
- At 1x a resolve writes the SDK's **shared memory**, which a vertex fetch reads. With resolution scaling (`draw_resolution_scaled`) a resolve writes only the **scaled resolve buffer**
  (`D3D12RenderTargetCache::Resolve`, `VulkanRenderTargetCache::Resolve`: read), and `TextureCache::MarkRangeAsResolved` marks the pages as scaled and as written by the GPU
  (valid) in the shared memory without writing it (read). A texture fetch of that memory is served from the scaled buffer, so the simulation itself keeps running on its scaled
  state; a vertex fetch sees whatever the shared memory holds, which is zeros or the start-up data.

## The trace (measured: `SP\raintrace\out\SUMMARY.txt`, the chain of 2026-10-28 06:30 to 06:53, 19 steps, all valid; the layer `rain-trace.sdk.patch` below)

| | 1x (D3D12) | 2x (D3D12, no fix) | 3x (D3D12, no fix) |
|---|---|---|---|
| the rain draw (vs `11AAD997`), traced draws | 24 | 24 | 24 |
| the GPU's statistics of the draw | clipper in 16,384, **out about 4,900**, pixel shader invocations **about 520,000** | clipper in 16,384, **out 0**, pixel shader **0** (the three draws listed) | out 0 in two of the three listed, pixel shader 0 in all three |
| the vertex buffers' first 16 bytes in the shared memory | data (`3F74CF55...`, `C4233D6E...`) | **zeros** (three of the four; the fourth had the start-up data in guest memory only) | zeros (the fourth buffer had the start-up data in both) |

- With the readback rule (the earlier attempt: `readback_resolve_rules` with `len=131072,pitch=128,height=128,fmt=37`, modes `full` and `fast`, and the same four resolves by address), the resolve log showed the
  rule matching the four resolves every frame and the data arriving in the **guest's** memory (`guest 4049BCE1...`), but the shared memory at the draw stayed zero in all but a few traced draws, and the
  statistics stayed at clipper out 0, pixel shader 0. Two arms whose rain number said "rain back" (`f2d-d3d12-x2-addr`, `f2d-d3d12-x2-full-b`, the last pair of shots) are scene changes, not rain: the
  picture sheet `rain-sheet-x2.jpg` shows the whole picture changing and no streak in any 2x or 3x picture, and the last shots of `full-b` show the race under way (cars driving, camera moving). So a CPU readback
  is the wrong path for data that the GPU consumes: it did not reach the shared memory before the draw, and it costs a GPU wait.
- `d2d-d3d12-x2-off-nodepth` (the rain drawn without a depth test) and `d2d-d3d12-x2-off-reupload` (its vertex buffers loaded from guest memory again at every draw): no rain either. Not the depth test;
  the start-up data in guest memory is not rain.
- Not the occlusion queries, the frame rate, or the render markers (the chain of `rain17fix`); `s1d-d3d12-x1-skip-rain` removes the streaks at 1x, `s1d-d3d12-x1-skip-bolt` does not (the bolt is another draw).

## The fix: write the unscaled version of those resolves into the shared memory, on the GPU

SDK patch `patches/rexglue-resolve-unscaled-copy.patch`, the option `resolve_unscaled_copy_rules` (default empty = off; changes during play): the rules of `readback_resolve_rules` (`addr`, `len`,
`pitch`, `height`, `fmt`, `src`, `phase`; the `mode` is not used). With draw resolution scaling, a resolve that matches a rule is, after the resolve itself, **downscaled to the 1x layout by the pass
that the readback uses (`resolve_downscale`, a compute shader) and the result is written straight into the destination range of the shared memory** (D3D12: the shader's destination is a raw UAV of the shared memory buffer;
Vulkan: a storage buffer descriptor of the shared memory buffer). There is no copy through a readback buffer, no wait, no CPU access. At native resolution the code is not reached (and with the option
empty, `IssueCopy` takes the same fast path as before).

What keeps the bookkeeping consistent (read from `SharedMemory`, `TextureCache`):
- The pages of the range were marked valid and written by the GPU by the scaled resolve itself (`MarkRangeAsResolved`, then `RangeWrittenByGpu`), so a later vertex fetch's `RequestRange` finds them valid and uploads nothing
  from guest memory; the CPU data that was uploaded before, or never, is not written over our data unless the game's CPU writes the pages (the same as at 1x). The copy comes after that call in the command stream, and before any
  draw that follows.
- The sparse host memory behind the range is allocated before the write (`SharedMemory::EnsureRangeAllocated`, new, public): `RequestRange` has a fast path (`rexglue-shared-memory-request-fast-path.patch`) that returns at
  once when every page is valid, which a range marked valid by a scaled resolve is, whether or not its memory was ever allocated.
- The scaled pages stay marked as scaled resolves (`scaled_resolve_pages_`), so the texture cache still loads a texture of that memory from the scaled buffer, as before. Only the vertex fetch sees the new copy.
- The write is ordered with the next reader through the shared memory's own state tracking (`UseForWriting` / `MarkUAVWritesCommitNeeded` on D3D12, `Use(kComputeWrite, range)` on Vulkan), like the 1x resolve.
- Checks before the copy (any failure logs `not done: <reason>` and leaves the resolve as it was): the destination is whole 32x32 tiles on a 128-byte boundary and 8, 16, 32 or 64 bits a pixel, the written range lies in
  the scaled resolve range the resolve made current, and the shared memory could be allocated.
- Log lines: `resolve_unscaled_copy_rules: N rule(s) from "..."` and `unscaled resolve copy: ... -> done (N done so far)` (the first 16, every 1024th, and all in a `resolve_log_frames` window).

The game option `mcla_rain_unscaled_copy` (default on; next start) sets the default of `resolve_unscaled_copy_rules` to `len=131072,pitch=128,height=128,fmt=37` **only when `resolution_scale` (or
`draw_resolution_scale_x` / `_y`) is above 1** (`src/perf_options.cpp`, like the vinyl rule); an own `resolve_unscaled_copy_rules` in `mcla.toml` or on the command line wins. In the earlier frame logs no other
resolve has that length, pitch, height and format (read: the 1x log has four of 128x128 with the format 37 and 17 of 128x128 with the format 6). The earlier attempt `mcla_rain_readback` is removed (it was
never in a release).

## What the downscale takes (read; it applies to the vinyl readback as well)

The pass takes, for each guest pixel, the host pixel at slot `pixel * scale_x * scale_y` of the scaled data (the top-left of its block; `readback_resolve_half_pixel_offset` moves it to the block's centre). The scaled
layout of this SDK (`XeTextureScaledTiledOffset` in the xenia sources the SDK's shaders are built from, `texture_address.xesli`) keeps host pixels in sub-units of N x 1 (N = 2 for 8 bytes a pixel, 4 for 4 bytes,
8 for 2 and 1), column-major inside one guest unit, not as one contiguous block per guest pixel. Worked out by hand from that function (not run): for 8 bytes a pixel at 2x2 the pass's slot is exactly the top-left host
pixel; at 3x3 it is a host pixel inside the right guest pixel's block, but one row down; for 4 bytes a pixel (the vinyl texture, `8_8_8_8`) at 2x2 every second guest pixel takes the value of its left neighbour
(host pixel (0,1) of the left pixel's block). A particle's state is probably the same over a block (inferred: the update pass samples the scaled state textures at the guest texel), so the rain's data should not be
affected at 2x or 3x; the vinyl readback at 2x and 3x may be (not measured; it was checked at 1x only, where nothing is downscaled).

The trace of the 2x readback runs showed the first two texels of all four vertex buffers equal in the guest's memory (`4049BCE1C198715E4049BCE1C198715E`), while the 1x data (shared memory) has them different. Not
explained yet. The layer prints 32 bytes now (four texels) of each buffer; the chain's fix arms show whether the shared memory has the same pairs, and one arm turns the half-pixel offset on.

## How xenia-canary handles it (read: the `canary_experimental` branch, commit `48b318a` of 2026-10-27; nothing of it was run)

- It has no GPU-side route from a scaled resolve to the shared memory either. Its answer for a title that reads resolved memory outside the GPU's texture path is `readback_resolve` (CPU readback, the same
  setting as here, default `none`): `D3D12CommandProcessor::IssueCopy_ReadbackResolvePath` downscales a scaled resolve with a compute pass and copies it to guest memory, and the description of
  `readback_resolve_half_pixel_offset` says it "matters when the CPU reads the resolve as data instead of an image, such as for gamma correction readbacks, occlusion checks, and GPU buffers reused later in the
  frame". Its downscale shader was rewritten for a scaled layout of its own (group-packed, `resolve_downscale.cs.xesl`), which this SDK does not have.
- Its other answer is `draw_resolution_scale_threshold` (default 0 = off): render targets whose pitch is at or below the value are not scaled at all and are resolved at 1x straight into the shared memory
  (`copy_native` in `D3D12RenderTargetCache::Resolve`); the description says small offscreen surfaces "often break when upscaled". That would keep the 128x128 particle targets native, with exact data and no
  copy. It needs render targets of two scale classes and the transfers between them (`scale_native` in its `RenderTargetKey`), which this SDK does not have, so it is not what this patch does.

## The chain and what is not known

`SP\rainfix2\chain.ps1` (the main session queues it): 2x and 3x on D3D12 and Vulkan with the fix off and on (the rain number and the picture sheets; the trace's shared-memory bytes and statistics), 1x unchanged,
and the cost at 2x with PresentMon, A-B-B-A. Until it has run: not known that the rain comes back on either API at 2x and 3x, that the downscaled data is good enough, what it costs, that the Vulkan path
has no validation error. Nothing was built or run for the PS5 or Linux (the same SDK code and the same defaults; CI compiles the host code for Linux).

## The development layer (`rain-trace.sdk.patch`)

For the chain. `launcher/dev/rain-trace.sdk.patch` applies to the patched SDK (`git apply`, after `rexglue-resolve-unscaled-copy.patch`), changes nothing unless a cvar is set, and works on both APIs:

- `tmp_draw_trace=<vertex shader hash prefixes, comma separated>` (hot-reloadable; a new value starts a new request): the first `tmp_draw_trace_count` (6) draws of each named vertex shader are
  traced. On Direct3D 12 a line `RAINTRACE #n vs=... frame=... scale=... guest_vertices ... viewport ... scissor ... | IAVertices ... VS ... GS ... ClipperIn ... ClipperOut ... PS ... | vf<n>@address size ...
  guest=<32 bytes> gpu=<32 bytes>` (the pipeline statistics of the draw from a query, and the first bytes of each vertex buffer in the guest's memory and in the shared memory, read back after the draw: the
  command thread waits for the GPU for every traced draw); a draw that is left out early gives `NOT DRAWN, left IssueDraw at step N`. On Vulkan one line `RAINTRACE (Vulkan) ... drawn, host_vertices ...`
  or the same `NOT DRAWN`.
- `tmp_draw_skip=<prefixes>`: those draws are left out. `tmp_draw_trace_nodepth=true`, `tmp_draw_trace_reupload=true` (D3D12): see above.
- Dev pad lines that read the game's state: `peeku [[0x8288B9AC]+40] 188` (the rain object), `peekf 0x827E1B10 32` (the weather table), `peeku 0x827E5640 1` (the render thread's weather type).

CI compiles the layer as an SDK patch on a throwaway branch (it is not in `patches/` of the fix branch).

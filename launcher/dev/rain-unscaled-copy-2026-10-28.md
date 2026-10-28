# Rain not drawn at resolution scale 2x and 3x (fork issue #17), 2026-10-28

Report: the rain is not drawn above native 720p (`resolution_scale` 2 or 3, Direct3D 12 and Vulkan, day and night); at 1x it is. The reporter's pause-menu
frame-rate report is another note.

**State (2026-10-28): the cause is confirmed by a trace run (D3D12, RTX 5080). The fix, `resolve_unscaled_copy_rules` (an SDK patch) with the game option
`mcla_rain_unscaled_copy`, was run by the chains `rainfix2` (the first version) and `downscale` (the batched version that ships): the rain is back at 2x and 3x on D3D12 and
Vulkan and no cost was measurable. Every claim below says whether it was read, measured or inferred.**

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

## What the downscale takes (checked by arithmetic: `launcher/dev/scaled-resolve-layout-check.py`; it applies to the vinyl readback as well)

The pass takes, for each guest pixel, the host pixel at slot `pixel * scale_x * scale_y` of the scaled data (meant as the top-left of its block; `readback_resolve_half_pixel_offset` moves it to the block's
centre). The scaled layout of this SDK (`XeTextureScaledTiledOffset` in the xenia sources the SDK's shaders are built from, `texture_address.xesli`) keeps host pixels in sub-units of N x 1 (N = 2 for 8 bytes a
pixel, 4 for 4 bytes, 8 for 2 and 1), column-major inside one guest unit, not as one contiguous block per guest pixel. The script transcribes that function and the tiled address and asks, for a 64x64 texture,
where the pass's read lands (run 2026-10-28):

| bytes a pixel | scale | top-left host pixel of the block | a host pixel of the pixel's own block |
|---|---|---|---|
| 8 (the rain's resolves) | 2x2 | 4096 of 4096 | 4096 of 4096 |
| 8 | 3x3 | 2048 of 4096 | 4096 of 4096 |
| 4 (the custom vinyl texture, `8_8_8_8`) | 2x2 | 2048 of 4096 | 2048 of 4096 |
| 4 | 3x3 | 1024 of 4096 | 3072 of 4096 |
| 2 | 2x2 / 3x3 | 1024 / 512 | 1024 / 2048 |
| 1 | 2x2 / 3x3 | 1024 / 512 | 1024 / 2048 |

So for the rain the data comes out as the simulation wrote it at 2x, and at 3x from a host pixel of the right block (the state is probably the same over a block: inferred), but the vinyl readback at 2x and 3x
reads a neighbour's pixel for half or a quarter of the pixels (every second pixel repeats its left neighbour at 2x; not looked at in a picture; the vinyl fix was checked at 1x, where nothing is downscaled). A pass
that is right for every size and scale (the unit-based inverse in the script matches the top-left host pixel for 4096 of 4096 in every row above) needs a new shader; xenia-canary has one for its own layout.

The trace of the 2x readback runs showed the first two texels of all four vertex buffers equal in the guest's memory (`4049BCE1C198715E4049BCE1C198715E`), while the 1x data (shared memory) has them different. The
pass is exact for these resolves at 2x (table), so the equal pair is in the scaled data itself (the simulation at 2x), not made by the pass. Not explained yet. The layer prints 32 bytes now (four texels) of each
buffer; the chain's fix arms show whether the shared memory has the same pairs, and one arm turns the half-pixel offset on.

## How xenia-canary handles it (read: the `canary_experimental` branch, commit `48b318a` of 2026-10-27; nothing of it was run)

- It has no GPU-side route from a scaled resolve to the shared memory either. Its answer for a title that reads resolved memory outside the GPU's texture path is `readback_resolve` (CPU readback, the same
  setting as here, default `none`): `D3D12CommandProcessor::IssueCopy_ReadbackResolvePath` downscales a scaled resolve with a compute pass and copies it to guest memory, and the description of
  `readback_resolve_half_pixel_offset` says it "matters when the CPU reads the resolve as data instead of an image, such as for gamma correction readbacks, occlusion checks, and GPU buffers reused later in the
  frame". Its downscale shader was rewritten for a scaled layout of its own (group-packed, `resolve_downscale.cs.xesl`), which this SDK does not have.
- Its other answer is `draw_resolution_scale_threshold` (default 0 = off): render targets whose pitch is at or below the value are not scaled at all and are resolved at 1x straight into the shared memory
  (`copy_native` in `D3D12RenderTargetCache::Resolve`); the description says small offscreen surfaces "often break when upscaled". That would keep the 128x128 particle targets native, with exact data and no
  copy. It needs render targets of two scale classes and the transfers between them (`scale_native` in its `RenderTargetKey`), which this SDK does not have, so it is not what this patch does.

## What was not known when the fix was written, and what the chains showed

Not known then: that the rain comes back on either API at 2x and 3x, that the downscaled data is good enough, what it costs, that the Vulkan path works. Known now: the rain comes back on
both APIs at 2x and 3x (the chain `rainfix2`, below, and the chain `downscale`, below the next heading); the cost is not measurable. Still not known: whether the downscaled state keeps the simulation
right over a long rain (the runs are about 25 s), the batched version at Vulkan 3x, a weaker GPU. Nothing was built or run for the PS5 or Linux (the same SDK code and the same defaults; CI compiles the host
code for Linux).

## What the chain `rainfix2` measured (2026-10-28, RTX 5080; `SP\rainfix2\out\SUMMARY.txt` of the session)

- **The rain is back with the fix** at 2x and 3x on both APIs: the share of the smooth pixels that changed in three frames (rain 0.10 to 0.16 at 1x, none 0.003 to 0.02): D3D12 2x 0.003 without the fix and 0.159 with it, 3x 0.011 and 0.183; Vulkan 2x 0.004 and 0.177, 3x 0.017 and 0.166; 1x 0.138 (D3D12) and 0.148 (Vulkan), where the fix is not reached (no `resolve_unscaled_copy_rules` line, no copies). The trace: the rain's draw rasterised 1,915,879 pixels at 2x and 4,279,787 at 3x with the fix and 0 without. (The rain at 1x: 517,206.)
- **The cost of the first version** (PresentMon, uncapped, fixed 1/60 s a frame, the car standing, 16 s a run): D3D12 2x A-B-B-A: GPU busy median +3.4% (5.348 against 5.175 ms), frame time median +3.3%; B-A-A-B +3.0%; all eight +3.2% (spread of equal arms 4 to 5%); with the rain's draw left out +3.4% (5.305 against 5.129 ms: the copies alone, about 0.17 ms); Vulkan 2x +2.0% GPU busy (one pair). The rain's own draw adds nothing measurable beyond that (+3.4% with it and without it).
- **Why the copies cost that much, and the batch** (`resolve_unscaled_copy_batch`, this branch): see `patches/README.md`'s section of the patch. Whether the game issues the four resolves when it is not raining, and what the batch costs: the chain `downscale`, below.

## What the chain `downscale` measured (2026-10-28, RTX 5080; `SP\downscale\out\SUMMARY.txt`, sections 3 to 5; the build had both the batched fix and the vinyl fix)

- **The rain with the batch** (the shipped version; the share of smooth pixels that changed in three frames, best pair of four, rain 0.10 to 0.16 at 1x, none 0.003 to 0.02): D3D12 2x 0.138, D3D12 3x 0.178, Vulkan 2x 0.171, all rain; the
  four copies logged as done (23,552, 16,384 and 21,504 by the last count logged), no `not done` line. Vulkan 3x was not run with the batch.
- **The cost** (PresentMon, uncapped, fixed 1/60 s a frame, the car standing, the rainy race). One session per API, the copies switched by the dev pad between segments of 10 s (A = copies off, Bo = first version, Bn = batch;
  order A Bo Bn Bn Bo A Bn Bo A A Bo Bn), GPU busy median: D3D12 2x A 4.497 ms (4.167 without its first segment, which was still warming: 5.487), Bo 4.161, Bn 4.145 (equal segments differ by about 2%); Vulkan 2x
  A 5.136 (4.901 without the first), Bo 4.794, Bn 4.762 (the whole session drifts up by 10%, so only the D3D12 session can say anything). Separate runs of 16 s on D3D12 2x (the method of `rainfix2`, two runs per arm):
  A 5.233 and 5.033 ms, Bn 5.207 and 5.130, Bo 5.054 and 5.274; the arm medians 5.133 (A), 5.168 (Bn, +0.7%), 5.164 (Bo, +0.6%). Two runs of one arm differ by up to 4%. So: the +3.2% that `rainfix2` measured for the first version
  was not found again, and the batch is not measurably cheaper than the first version. (Earlier chains saw this PC run at two different speeds from one run to the next; compare inside a session or a block.)
- **Outside rain:** the four resolves (length 131,072, pitch 128, height 128, format 37) are in the resolve logs of the three rainy runs (8 lines each) and in none of the clear vinyl races at 1x, 2x and 3x on both APIs (33 distinct
  classes each) nor in 40 s of free roam at 2x (22 classes; the weather of that run was not recorded). So the game does not issue them in the clear races, and the copies cost nothing there; a rainy free roam was not run.


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

# Rain not drawn at resolution scale 2x and 3x (fork issue #17), 2026-10-28

Report: the rain is not drawn above native 720p (`resolution_scale` 2 or 3, Direct3D 12 and Vulkan, day and night); at 1x it is. The reporter's pause-menu
frame-rate report is another note.

**State: the cause was found from the game's recompiled code, its frame logs and the SDK's source (nothing of the fix has run yet; the chain that runs it is in
the notes of the agent "raintrace"). Every claim below says whether it was read or measured.**

## What the earlier notes got wrong

The earlier notes took the draw with the vertex shader `284A04E0` (5,952 indices, a 128x128 DXT1 texture, first in render phase `0x02000000`) for the rain,
because it is in the 1x frame logs and not in the 2x ones. It is the lightning bolt:

- Read: the image has the strings `LightningTexture`, `lightning.dds` (in `tune/lighting/textures`), `LightningPulseIntensity`, `LightningPulseFrequency`,
  `m_lightningColor1/2`; the vertex shader fetches a 3D mesh (three floats and a packed normal a vertex) and reads a table of points and jitter from the
  constants `c[100 + a0]`; the constants `c16` and `c17` hold its two end points; the weather object (`sub_82746B78`, 1,872 bytes, `world + 52`) updates a pool of
  8 timed entries of 96 bytes (`sub_827481C0`, called after phase `0x02000000` by `sub_823069C0`) and draws each active one (`sub_82747810`). A bolt is a random event,
  so a pair of 2-frame logs without it (2x) against a pair with it (1x) proves nothing.
- Not measured yet: that no bolt is drawn at 2x over a longer time (the chain traces the bolt's draws in every run).

The rain is the other draw: **32,768 vertices, prim 13 (a quad list: 8,192 quads), vertex shader `11AAD997`, pixel shader `47213022`**, textures 32x32 and 256x64 (both
`DXT1`), the last draw before the scene's resolves at the end of phase `0x04000000`. It is in the frame logs at **1x and at 2x** (`C:\mcv\rain17\fl\p1d-d3d12-x1-gt\rain_0.txt`
draw 2873, `p2d-d3d12-x2-gt\rain_0.txt` draw 2897; measured), with the same state, so the game does draw it at 2x.

- Read: `sub_822D2350` (slot 1 of the vtable of the class the image names `mcGameDebugDraw`, at `0x8203BD3C`) calls `mcRain::Draw` (`sub_822EE700`) with 8192 for the world's
  rain object (`[[0x8288B9AC] + 40]`, 752 bytes, made by `sub_823047D8` and filled by `sub_822EF0D8`); `Draw` draws `count * 4` vertices as a quad list. Its gates are the
  intensity at `+732` (set each frame by `sub_822EFF70` from the weather table at `0x827E1B10`) and the flag at `+5`.

## What differs at 1x and 2x: the rain's particles are simulated on the GPU

- Read (`sub_822EF0D8`, `sub_822EE700`) and measured (frame log, 1x, draws 32 and 33): the rain object keeps 16,384 drops as **four 128x128 `k_32_32_FLOAT` render
  targets** (the image's strings: `Particle Pos XY`, `Particle Pos ZW`, `Particle Vel XY`, `Particle Vel ZW`, the techniques `UpdateParticles` and `RenderParticles`,
  `ParticleCollision`). One draw (a quad, four colour targets, pixel shader `20F6F275`, reading the four state textures, a noise texture and the collision map)
  updates them in the first phase, and **four resolves** write them to four 128 KB textures: `0x0561F000`, `0x0563F000`, `0x0565F000`, `0x0567F000` (length 131072, pitch
  128, height 128, copy destination format 37).
- Read: the same memory is **the rain's vertex data**. `sub_822EF0D8` locks each of the four textures (`r31 + 84` ... `+ 96`) and makes a vertex buffer object from the locked
  address (`sub_821B4838`, results at `r31 + 632` ... `+ 644`, 16,384 entries) and `Draw` binds them as streams 0 to 3; the vertex shader `11AAD997` fetches `vf0` to `vf3` as `FMT_32_32_FLOAT`,
  stride 2 dwords. The console's render-to-vertex-buffer trick. (The reading of the object creation is the least certain step; the trace's `vf` lines give the fetch constants' addresses, which should be
  the four addresses above.)
- Read (`D3D12RenderTargetCache::Resolve`): with `draw_resolution_scaled` a resolve writes the SDK's **scaled resolve buffer** only (`EnsureScaledResolveMemoryCommitted`,
  `MakeScaledResolveRangeCurrent`, `MarkRangeAsResolved`); the shared memory, which a vertex fetch reads, is not written. At 1x the resolve writes the shared memory. A texture
  fetch of the same memory at 2x is served from the scaled buffer (`IsRangeScaledResolved`), so the simulation itself keeps running on its own scaled state; only the vertex
  fetch sees the stale bytes the CPU wrote at start-up. The Vulkan code is the same design.
- Consequence (inferred): the rain's vertex shader gets the start-up values at every frame (no motion, or no drops where the start-up values are zeros).

This fits every earlier observation: the rain draw exists at every scale; 1x only; both APIs; day and night; none of the fork's resolve and texture options (they act on texture
fetches and on resolves, not on a vertex fetch of resolved memory); not the frame rate; not the occlusion queries.

## The fix tried: read the four resolves back

`readback_resolve_rules` (`patches/rexglue-resolve-readback-rules.patch`, the mechanism of the vinyl fix) reads back the resolves that match a rule: the resolved data is
(at 2x and 3x downscaled to the 1x layout by a compute pass, then) copied into the guest's memory, which invalidates the shared memory's pages, so the next vertex fetch loads the
fresh bytes. The rule of the four resolves: `len=131072,pitch=128,height=128,fmt=37` (no other resolve of the logs has that length with that pitch, height and format; the 1x log has four
resolves of 128x128 with the format 37 and 17 of 128x128 with the format 6, 64 KB).

The game's option `mcla_rain_readback` (`full` by default, `fast`, `off`; next start) puts the rule into the default of `readback_resolve_rules` **only when `resolution_scale` (or
`draw_resolution_scale_x` or `_y`) is above 1**, joined to the vinyl rule with `;` (`src/perf_options.cpp`); an own `readback_resolve_rules` still wins. At 1x nothing changes: the code is not
reached. `full` waits for the GPU after every one of the four resolves, every frame (the four resolves are in every frame of the rainy logs; whether a dry race has them was not
looked at; `mcRain::Update`, `sub_822EFF70`, stops early when the intensity is 0, so the simulation may not run then: read, not checked); `fast` copies the previous frame's data without waiting (the rain is drawn one frame late; the simulation then reads one more frame of lag from the guest's memory, which is a
fixed delay, not a drift: inferred, not measured).

**Not measured:** that the rule matches (the SDK's resolve log names the rule: `set resolve_log_frames 3`), that the rain comes back on D3D12 and Vulkan at 2x and 3x, what `full` and `fast`
cost, that the downscaled state is good enough for the simulation, nothing on Linux or the PS5.

## The development layer (`rain-trace.sdk.patch`)

For the chain. `launcher/dev/rain-trace.sdk.patch` applies to the patched SDK (`git apply`, or `gt-build.ps1 -Layer rain-trace`), changes nothing unless a cvar is set, and works on both APIs:

- `tmp_draw_trace=<vertex shader hash prefixes, comma separated>` (hot-reloadable; a new value starts a new request): the first `tmp_draw_trace_count` (6) draws of each named vertex shader are
  traced. On Direct3D 12 a line `RAINTRACE #n vs=... frame=... scale=... guest_vertices ... viewport ... scissor ... | IAVertices ... VS ... GS ... ClipperIn ... ClipperOut ... PS ... | vf0@address size ...
  guest=<16 bytes> gpu=<16 bytes>` (the pipeline statistics of the draw from a query, and the first bytes of each vertex buffer in the guest's memory and in the shared memory, read back after the
  draw: the command thread waits for the GPU for every traced draw); a draw that is left out early gives `NOT DRAWN, left IssueDraw at step N` (steps: 1 the shaders analysed, 2 submission begun, 20 and 30
  primitive processing, 3 processed, 35 render targets, 4 render targets updated, 45 pipeline, 55 waiting for an asynchronously created pipeline (the draw is left out until it is ready), 6 textures,
  75 and 8 bindings, 9 vertex buffers). On Vulkan one line `RAINTRACE (Vulkan) ... drawn, host_vertices ...` or the same `NOT DRAWN`.
- `tmp_draw_skip=<prefixes>`: those draws are left out (what the picture loses is what they draw).
- `tmp_draw_trace_nodepth=true`: the traced shaders' draws are drawn without a depth test (D3D12). `tmp_draw_trace_reupload=true`: their vertex buffers are loaded from guest memory again for every draw (D3D12).
- Dev pad lines that read the game's state: `peeku [[0x8288B9AC]+40] 188` (the rain object), `peekf 0x827E1B10 32` (the weather table), `peeku 0x827E5640 1` (the render thread's weather type).

CI compiles the layer as an SDK patch on the throwaway branch `ci/raintrace` (it is not in `patches/` of the fix branch).

## Tools that found it

`launcher/dev/frame-log.sdk.patch` (the frame logs: draws with shaders, textures, resolves), the recompiled sources of a build (`generated/default`, grepped for the vtable, the callers and the constants; the
image's RTTI and strings from the dev pad's memory dump), the shader dumps of the SDK's `dump_shaders` (the Xenos disassembly of the two rain shaders and the bolt's), and the SDK's source.

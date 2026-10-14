# Profiling, 2026-10-14

Windows 11, RTX 5080, Ryzen 7 9800X3D, D3D12, the fork's build at `ea4456f` plus
the instrumentation below. Title screen and its flyover (no gameplay: getting
into a race needs a player). The title scene is picked at random per launch, so
anything compared across launches is only indicative; the A/B results below
switch a cvar inside one run.

## CPU, native resolution, uncapped

Built-in sampler (`--mcla_profile`) against a RelWithDebInfo SDK (PDBs), ntdll
resolved from its public symbols; thread CPU time sampled over the same 15 s.

Launcher defaults (heavy view, 7,900 draws a frame, 114 FPS):

| Thread | CPU | Top items |
|---|---|---|
| Main guest thread | 98% of a core | guest code 84%; GPU-fence spin `sub_82788F80` 8%; waits about 7%; `ZwProtectVirtualMemory` 2.9% (write faults) |
| GPU command thread | 95% | **`ZwProtectVirtualMemory` 18.7%**, `UpdateBindings` 7.4%, waits 7.7%, `ExecutePacket` 3.6%, `IssueDraw` 2.6%, `UpdateSystemConstantValues` 2.5%, `RequestRanges` 2.2%, transfers 2.2% |
| GPU Submission (worker) | 33% | waits 70%, NVIDIA driver 22%, D3D12Core 5% |

Counters (temporary, `upload-counters.sdk.patch`): about **190,000 page
protection changes a second** (134,000 re-watching uploaded pages on the GPU
command thread, 57,000 unprotecting on write faults), **12,000 write faults a
second** (about 85 a frame), 1.2-1.8 GB/s uploaded, all of it pages invalidated
by write faults (3 GB/s invalidated). With `clear_memory_page_state=false` the
per-frame re-upload is gone, but the game rewrites its dynamic data every frame
and each rewrite faults, invalidates 64 pages, and is uploaded and re-protected
draw by draw.

With hot pages (`--shared_memory_hot_page_faults=4 --shared_memory_hot_page_ms=10000`)
and `rexglue-shared-memory-fast-path-hot-pages.patch`: faults about 200 a
second, protection changes 1,000-2,000, uploads about 0.1 GB/s. Then the GPU
command thread is 82% busy (`UpdateBindings` 7.7%, waits 15%, the rest spread
thin, nothing over 5%) and the main guest thread waits 28% on the GPU (its own
fence wait). On light views the frame rate sits at about 146 FPS whatever the
GPU thread does: both threads wait on each other at the game's per-frame fence,
and neither has a single large cost left.

## In-run A/B (native, uncapped)

| Change | Result | Kept |
|---|---|---|
| Hot pages on (4 faults, 10 s) vs off, with the fast-path patch | GPU command thread **5.6 vs 7.0 ms CPU per frame** (-20%); heavy view 122 -> 146 FPS; frame time per draw -9.8% +/-3.5 (halves -1.2 / -15.5, the first half at the 146 FPS ceiling) | recommended for the launcher's defaults, needs a visual check in play |
| `shared_memory_request_fast_path_with_hot_pages` on vs off (hot pages on) | full-path requests 1.45M/s -> 110k/s; GPU command thread 6.1-6.5 -> 4.9-5.4 ms per frame (-17%) | yes: `rexglue-shared-memory-fast-path-hot-pages.patch` |
| D3D12 byte-exact hot-page uploads (`shared_memory_hot_page_exact`, port of the Vulkan path) | uploads 455 -> 113 MiB/s, GPU command thread 5.57 vs 5.62 ms per frame (no difference) | no: `dropped-patches\rexglue-d3d12-hot-page-byte-uploads.patch` |
| `shared_memory_invalidation_pages_log2=4` (hot pages on) | frame time identical (146 FPS ceiling), per-draw -5.2% +/-2.8 noise | no |
| `shared_memory_request_granularity_log2=16` (hot pages on) | frame time identical, per-draw +1.1% | no |

Backend: the fast-path patch and the hot-page settings are backend-independent
(the PS5 already runs hot pages and widened requests, so the patch matters there
on every frame). The byte-exact upload path already exists for Vulkan.

## GPU at 3x (D3D12, launcher defaults, GPU 99.8% busy)

Timestamps (temporary, `gpu-timestamps.sdk.patch`, one frame every 4 s, about
1% overhead) bracketing ownership transfers between EDRAM render targets
(`xfer`, with resolve clears), resolves (`IssueCopy`), texture loads and
uploads; the rest is the game's draws. PIX GPU captures work
(`WinPixGpuCapturer.dll` loaded at start, `CaptureNextFrame`), but every PIX
analysis (event list, counters) needs Windows Developer Mode, which was not
turned on.

| Share of GPU frame time | 3x (12.0 ms, 83 FPS) | 2x (8.8 ms, 114 FPS, GPU 100%) |
|---|---|---|
| Game draws | **32%** | 51% |
| EDRAM ownership transfers | **32%** | 19% |
| Resolves | **17%** | 11% |
| Texture loads | **15%** | 8% |
| Uploads | 4% | 12% |

About two thirds of the 3x GPU time is emulation work that grows with the
resolution scale, not the game's shading. The ROV path
(`--render_target_path_d3d12=rov`, no ownership transfers) is far slower on this
card: 25.7 ms against 12.0. Native is CPU-bound (GPU 64%, 146 FPS).

Pass list of one 3x frame (consecutive draws to the same render targets merged;
`t` = EDRAM tiles of 80x16 samples, pitch 16t = 1280 pixels at 1x; times in ms; draw counts per pass are approximate):

```
  6  0.24   53 draws  d @0t <4t> 2xMSAA D24FS8, c0 @128t 2_10_10_10_FLOAT        small 2x targets (reflections?)
  7  0.48  135 draws  d @0t <13t> 4xMSAA D24FS8, c0 @416t 2_10_10_10_FLOAT
 11  0.56    4 draws  d @0t <7t> 4xMSAA, c0 @224t 8_8_8_8                          xfer 0.52
 12  1.25  898 draws  d @0t <16t> 4xMSAA D24S8 (depth only)                       shadow map; resolves 0.46
 14  0.90   28 draws  d @0t <7t> 4xMSAA, c0 @224t 8_8_8_8                          xfer 0.52
 15  0.57    1 draw   d @0t <16t> 4xMSAA, c0 @1024t 2_10_10_10_FLOAT               xfer 0.49
 16  3.22 1922 draws  d @0t <16t> 2xMSAA D24FS8, c0 @1024t 2_10_10_10_FLOAT        main scene; resolves 0.67, texture loads 0.50
 17  0.22    1 draw   same at 4xMSAA                                                xfer 0.20
 18  1.11 1922 draws  main scene targets again                                     resolves 0.27
 19  0.39    1 draw   c0 @0t <8t> 1x 2_10_10_10_FLOAT                               half resolution post
 22  0.10    6 draws  c0 @0t <1t> 1x 32_FLOAT                                       luminance
 24  0.28    1 draw   c0 @0t <16t> 1x 16_16_16_16_FLOAT                             full-size post
 25  0.39    2 draws  c0 @0t <4t> 1x 2_10_10_10_FLOAT                               xfer 0.30
 26  0.52    3 draws  d @0t <16t> 1x D24FS8, c0 @720t 1x 8_8_8_8                    final 8-bit target; xfer 0.31
 27  0.20    1 draw   d @0t <16t> 4xMSAA (depth only)                              xfer 0.19
 29  0.28   29 draws  d @0t <16t> 1x, c0 @720t 1x 8_8_8_8                           menu/HUD; xfer 0.28
 30  0.20    1 draw   d @0t <16t> 4xMSAA (depth only)                              xfer 0.19
 31  0.11    0 draws  final resolve
```

Single draws with a different MSAA mode on the same EDRAM range (passes 15, 17,
27, 30) each force a full transfer of the targets into a new host render target
and back, 0.2-0.5 ms each at 3x. If they are clears or full overwrites, the
transfer is wasted; skipping it would need to prove the draw overwrites
everything it covers.

## For the upscaler (from `upscaler-research.md`)

1. Pass list and shares: above. Transfers 32%, resolves 17%, texture loads 15% at 3x.
2. The HUD (here the title menu) draws into the final 1x `8_8_8_8` target at EDRAM
   base 720 tiles, after the `16_16_16_16_FLOAT` full-size pass and a 2-draw
   quarter-size pass; single 4xMSAA depth-only draws are interleaved with it.
3. Resolves happen inside the main scene pass (0.67 ms in pass 16): later scene
   draws sample resolved data.
4. Pixel-position shaders: not determined (needs shader inspection).
5. Main scene: `2_10_10_10_FLOAT` colour, `D24FS8` depth, 2xMSAA, pitch 16 tiles
   (1280 wide at 1x); `mcla_single_tile` on.
6. Phase bits to passes: not determined (needs a game-side hook).
7. Depth is `D24FS8` (has stencil); whether the vehicle id is in it: not determined.
8. GenVelocity: no velocity pass identified on the title screen.
9. 3x 12.0 ms (83 FPS) against 2x 8.8 ms (114 FPS), different random scenes, both GPU-bound.

Conclusion: rendering the scene at a lower internal resolution helps only as
far as the transfers, resolves and texture loads shrink with it. A cheaper
native 3x has the same target: those three are two thirds of the frame.

## Reproducing

- `upload-counters.sdk.patch` (SDK, on top of the fork's patches): logs
  protection changes, write faults, uploads and GPU command thread CPU time per
  frame every 5 s (`TMPPROF`/`TMPCPU` lines in the log).
- `gpu-timestamps.sdk.patch` (SDK): `MCLA_GT_EVERY=<s>` and `MCLA_GT_DIR=<dir>`
  write one timed frame as `gt_<n>.csv` every few seconds; `gt-analyze.py <dir>
  --passes 16` prints the shares and the pass list. It also carries the D3D12
  byte-exact upload path (harmless).
- Both are temporary instrumentation: apply, build, measure, reverse.

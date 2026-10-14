# MCLA upscaler feasibility (read-only research, 2026-10-14)

Verdict: feasible and better placed than most: MCLA already computes reprojection velocity for its motion blur. Workable route: keep the SDK scale at 3x, render only the 3D scene into a smaller sub-rectangle of the 3x targets, upscale it before post-processing and the HUD. Per-render-target scales and full per-object motion vectors are research projects.

## 1. Resolution scale in the SDK
- One global factor, fixed at startup: TextureCache::GetConfigDrawResolutionScale (pipeline/texture/cache.cpp:66-76, :237-244), passed to the render-target and texture caches (d3d12/command_processor.cpp:926-956), baked into EDRAM pitch/tile math (pipeline/render_target/cache.cpp:434-436, :806, :983), resolves, scaled resolve textures and the shader translators.
- Scene at 2x with post/HUD at 3x (per-target scales): not realistic; the 360 aliases EDRAM and resolved memory, so mixed scales need resampling at every ownership transfer.
- Feasible alternative: keep scale 3; host viewport/scissor and ndc_scale/ndc_offset are per draw (d3d12/command_processor.cpp:4069-4072), so scene draws can go into a sub-rectangle at any ratio (2/3 = DLSS Quality, 1/2 = Performance); upscale it in place to the full rectangle at the scene/post boundary.
- The presenter can't do it: IssueSwap gives it one frontbuffer with the HUD already in (command_processor.cpp:2119-2216).

## 2. Frame structure (from LARecomp notes; confirm with a capture)
- sub_822E6408 walks render-pass bits 1..2^29, dispatching 13 renderers per set bit (LARecomp hooks/ui.cpp:106-109). Renderer object at [0x8287E064]: +448 enable mask, +364 requested, +356 current phase (render_phases.cpp:158-161, :224-225).
- Scene: 720p, 2xMSAA, tiled (BeginTiledRendering sub_8217A470, r17=0 = main scene; config/render_perf.toml).
- Post: rage::grPostFX at 0x829054A0; motion blur setup sub_8260E9A0, technique StreakMotionBlur, quarter-res buffers (frame_capture.cpp:3270-3295); DoF composite sub_8260EBB8.
- Hookable boundary: the phase loop. A hook can emit a PM4 NOP with a magic payload, which the SDK already parses (command_processor.cpp:789); RT64's NOOP-encoded extended commands are the precedent. GPU-side fallback: detect the tonemap/composite and UI shaders by hash.
- Velocity data exists: motion blur reprojects with the previous frame's matrix; the scene writes a vehicle id (1-5) into stencil and Mc4MotionBlurVehicleMtxs gives each tagged vehicle its own matrix (render_target_pool.cpp:131-141). Wrong id: 123 px error on the player's car vs 2.5 px (depth_stencil_pack_cs.hlsl:13-27). GenVelocityNoVehicleBlur pass exists (constant_upload.cpp:36).

## 3. Motion vectors
- Camera-only: correct for the static world; player's car, traffic, wheels smear/ghost.
- Camera + the game's vehicle matrices: world plus up to 5 tagged cars correct; still wrong for untagged traffic, pedestrians, wheel spin, foliage sway, particles (FSR reactive mask helps).
- Ordering: the motion-blur constants reach the GPU after the upscale point; capture them game-side in sub_8260E9A0 or the camera code, tagged per frame.
- Per-object vectors: RT64's cross-frame draw matching is "highly experimental... prone to errors"; re-running vertex shaders with previous constants = translator changes + an extra velocity target outside EDRAM; months.

## 4. Integration
- FidelityFX is off: REXGLUE_ENABLE_FIDELITYFX defaults OFF, "experimental" (CMakeLists.txt:23); enabling fetches rexglue's FidelityFX fork at eee08db during configure (cmake/rexglue_fidelityfx.cmake:48-55); make_pc.ps1 never sets it.
- The existing fsr2/fsr3 option passes colour as both depth and motion vectors with reset=true every frame (d3d12_presenter.cpp:152-179, warning presenter.cpp:238-240): effectively spatial, and the HUD still gets scaled.
- FSR 3.1 via ffx-api (MIT, any GPU) and DLSS via NGX/DLSS SDK take the same inputs (colour, depth, motion vectors, jitter, exposure): one layer can serve both. DLSS DLL licence fine for a personal build. Jitter via ndc_offset on scene draws only. Depth from the host depth target (RTV path on NVIDIA).
- Spatial fallback: FSR1 EASU+RCAS bytecode already in the presenter (src/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_*).

## 5. Staged plan
- (a) HUD-separated spatial upscale, ~2-3 weeks, medium risk: phase marker, shrunk scene viewport/scissor, FSR1/RCAS (or NIS) at the boundary. Risks: shaders using pixel position (SDK divides by scale), mid-scene resolves sampled later, garbage outside the sub-rectangle. Quality: sharp HUD, softer scene than native 3x, foliage shimmer.
- (b) Temporal DLSS / FSR 3.1, ~3-5 more weeks, medium-high risk: jitter, vehicle-aware vector compute pass, game-side matrix capture, optional auto-reactive mask. Turn the game's MSAA off (mcla_msaa=false). Quality: likely near-native at Quality ratio, probably fixes the foliage dither; ghosting limited to untagged objects.
- (c) Per-object vectors: research, not recommended.
- Alternative: if most 3x GPU time is EDRAM/resolve/transfer overhead rather than shading, (a)/(b) save less; optimise the overhead for native 3x instead. Decide after profiling.

## Outcome of the 2026-10-14 profiling
See profiling-2026-10-14.md. At 3x the GPU is 99.8% busy and the game's own draws are only 32% of the frame; EDRAM transfers (32%), resolves (17%) and texture loads (15%) make up most of the rest, and many transfers come from switching MSAA modes on the same EDRAM range. A scene-only upscaler mainly shrinks the 32%, so cutting the emulation overhead for a cheaper native 3x is the better first target. `--mcla_msaa=false` alone cuts the 3x frame by 18-25% (13.2 -> 9.9 ms night city, 12.6 -> 10.2 ms Hollywood) at the cost of slightly more aliasing. Not started; any upscaler work waits for a decision with the user.

## Frame-capture questions (gameplay, 3x)
1. Ordered pass list: RT address/format/size, draw count, GPU time (shadows, opaque, transparent/particles, reflections, scene resolves, downsample, StreakMotionBlur, DoF, bloom, tonemap, HUD, frontbuffer resolve). Share of EDRAM transfers and resolves.
2. Where the HUD starts: target, shaders, same target as tonemap? Any 3D (minimap) after it?
3. Scene resolves sampled by later scene draws (refraction, soft particles, water).
4. Scene shaders reading pixel position / deriving UVs from screen position.
5. Scene target format (7e3?), MSAA, tile count with mcla_single_tile.
6. Phase bits (renderer+356) to passes.
7. Vehicle-id stencil present at end of scene; depth format.
8. GenVelocity output resolution/format; covers vehicles?
9. GPU frame time of the same view at 2x and 3x.

Sources: github.com/rt64/rt64; NVIDIA DLSS License Agreement (06.26.2020); NVIDIA RTX SDKs License (23 Jan 2023); Unigine forum topic 8984 (DLSS licensing and Streamline); NVIDIA RTX Remix runtime open source.

# How MCLA builds a frame: HDR and upscaler inputs (2026-10-16)

This note answers the frame-capture questions of `upscaler-research.md` and
the questions that HDR output and a HUD-separated upscaler depend on. It is
an investigation only: no product code changed. Windows, RTX 5080, D3D12,
launcher settings, built at `df4a1e5`.

## How it was measured

Two development patches, applied, built, used and reversed (both are in this
folder):

- **`render-markers.patch`** (game, `src/render_perf.cpp`, cvar
  `mcla_dev_render_markers`) writes a PM4 type-3 NOP (`'MCPH'`, kind, value)
  into the game's command buffer in three places:
  - where each render phase starts: an override of `sub_822E55D0`, which the
    phase loop calls with the phase bit in r4;
  - where the phase loop `sub_822E6408` starts and ends;
  - where the frame function `sub_822C0980` starts and ends, and the post
    functions `sub_8260EBB8` and `sub_8260E9A0`.

  The D3D device is `[0x82839254]`: `[device+48]` is the command buffer's write
  pointer (the last word written) and `[device+56]` its limit. The game's own
  writers check the limit once and then write up to 8 words (`sub_824199B0`).
  Markers are only written on the thread that calls `sub_822C0980`. A first
  version also marked `sub_82189E80` (a render-state setter called thousands
  of times a frame) and some frame-level callees. That corrupted the command
  stream (`ExecutePacketType0 overflow`), most likely by landing inside data a
  caller was still writing. The version kept marks only the functions listed
  above, and the 17 runs made with it were clean.
- **`frame-log.sdk.patch`** (SDK, D3D12, `TMPFL`; goes on top of
  `gpu-timestamps.sdk.patch`). Setting `tmp_frame_log` to a new label (the pad's
  `set`) logs the next `tmp_frame_log_frames` frames to
  `<tmp_frame_log_dir>/<label>_<n>.txt`. Each frame gets:
  - one `D` line per draw: primitive and count, `RB_SURFACE_INFO`, colour and
    depth info and masks, depth, stencil, blend and alpha-test state, scissor,
    viewport, `PA_CL_VTE_CNTL`, VS and PS ucode hashes (the `--dump_shaders`
    file names), whether the PS reads the pixel position (param gen), and every
    bound texture fetch (address, format, size, signs);
  - `VC`/`PC` lines with the float constants a shader reads, for the first draw
    of each shader and for every draw of 6 indices or fewer;
  - `R` lines for resolves, `B` lines for predicated-tiling bin selects, and
    `M` lines for the game's markers;
  - the swap (`S`), with the display gamma ramp (`G256`, `GPWL`).

  The same frame gets GPU timestamps, written to `<label>_<n>_gt.csv`. These
  carry the `gpu-timestamps` brackets plus a `D<n>` label per draw and the
  phase markers. With `tmp_frame_log_quiet=true` only the timestamps are
  written, so logging overhead does not distort the times; every time below
  comes from quiet frames.

  `tmp_frame_dump_ps=<hash>` writes the guest memory of every texture of the
  draws that use that pixel shader. Run with `--readback_resolve=full` so that
  resolved data is in guest memory when the draw runs. That is how the
  pre-tonemap image was read back.
- Runs: `devrun.py run` at 3x (and 1x/2x for scaling), D3D12. The day and night
  city runs set the time of day by poking `[0x8287E26C]+0x38CC/0x38DC`. The
  others were the race (`boot-to-race.pad` with `race.pad`'s follow route),
  the pause menu tabs, the GPS map, the replay, the title, the loading map
  zoom-out, the legal screens and the attract movie. In the timing runs,
  `--d3d12_threaded_submission_split_draws=0` makes a frame one submission, so
  the GPU never waits mid-frame on the recording thread at 1x and 2x.
  - The race at 1x and 2x still had two mid-frame gaps of 1-3 ms. Intervals
    over 0.5 ms are counted as idle.
  - Every run was clean in the GPU guard, and real save data was unchanged.
- Logs, screenshots (4K PNG) and contact sheets were left in the session's
  scratch folder (`frame-investigation\out\{city,race,menus,attract,replay2,dump,time1-3}`).
  They are not in the repository.

## Summary

- **Phase bits are the frame's skeleton.**
  - `0x1` to `0x400`: off-screen work (reflections, shadow cascades, impostors).
  - `0x800` to `0x04000000`: the main view.
  - `0x08000000`: post.
  - `0x10000000`: the tonemap, then additive light glows.
  - `0x20000000`: the HUD and UI.

  This holds in gameplay, the title, the loading map zoom-out, the pause
  menus, the GPS map and the replay. The legal screens skip the phase loop, and
  Bink movies are drawn after it.
- **The main scene is still drawn as two predicated tiles** (1280x512 and
  1280x208, about 1,370 draws replayed per tile). `mcla_single_tile` makes no
  difference: the same bins and resolves with it on and off in one run.
- **The tonemap is one draw.** Its PS is `E5EBEFAE4D165CBA`: Reinhard extended
  on luminance (DirectX SDK Yxy style), with:
  - exposure from a 1x1 adapted luminance, which the VS clamps to [0.15, 0.2];
  - bloom added after the curve;
  - saturation, colour balance and contrast set by constants;
  - an sRGB-like encode, `1.055·x^(1/2.2) − 0.055`, into a plain `8_8_8_8`
    target.

  The display then gets the console's gamma ramp,
  BT.709 OETF(sRGB EOTF(x)), which the SDK applies at present. There is no
  in-game brightness setting.
- **The scene has little range above the tonemapper's white point.** In shader
  units the maximum is 0.44 by day, 1.18 at dusk, 1.55 in the city at night,
  1.87 for the sun on the title screen and 1.99, the 7e3 ceiling, for
  emissives in the night race. The white point sits at 0.28-1.13. Much of
  what looks bright comes from bloom and from the light glows. Both are added
  after the tonemap, in display space.
- **The HUD is drawn into the same 1x `8_8_8_8` target as the tonemap.** It is
  alpha-blended, and includes a 3D speedometer mesh lit by the scene's
  environment map, plus render-to-texture minimap and gauge faces made in the
  post phase. In the menus (pause, GPS map) the UI phase is a 3-tile 4x MSAA
  pass that first copies the tonemapped scene in as a texture.
- **No separate velocity buffer.** The motion-blur shader computes per-pixel
  motion at half resolution from depth, the stencil vehicle ID (`ref = 4·k +
  {0,1}`) and per-vehicle matrices `c[69+4k..72+4k]`.
- **The scene's GPU time is mostly per-draw cost, not pixels.** Scene phases at
  1x/2x/3x: day 2.61/3.21/3.90 ms, race 5.76/6.32/6.66 ms. A scene-only
  upscale from 3x at 2/3 scale saves 0-0.7 ms before the upscaler's own cost.

## 1. The phases (city by day and the night race, 3x)

GPU time is the median of 8 quiet frames (city) and 24 (race), both tiles
summed, in ms.

| Phase | What it draws (targets at 1x scale) | Day | Race |
|---|---|---|---|
| 0x1 | 2 small draws to 128-256 px `8_8_8_8`/`8_8_8_8_GAMMA` targets (8 with depth in the race) | 0.07 | 0.18 |
| 0x2 | 256x256 7e3 (bias 2) reflection view, 2x/4x, about 60 draws → 16x4F | 0.12 | 0.28 |
| 0x4, 0x8 | 512x256 7e3 environment map (4x), its mip chain 256x128 to 64x32 → `032E2000` 16x4F (car paint and HUD gauge use it) | 0.95 | 0.72 |
| 0x10 | race/replay only: about 800 depth-only draws, 160 and 320 px wide (not identified) | - | 0.56 |
| 0x20, 0x40, 0x80, 0x100 | daylight: four 640x640 4x `D24S8` sun cascades → 1280x1280 atlas; impostor tiles (128-256 px `8_8_8_8`, 4x) | 1.78 | 0.41 |
| 0x400 | 256x256 `8_8_8_8` 4x shadow texture ("night/blend" shadow) | 0.34 | 0.55 |
| 0x800 | main view starts: clear rectangles of the scene targets; without sun shadows (night, race) also the depth-only pre-pass, about 210-250 draws per tile | 0.03 | 0.82 |
| 0x1000 | daylight only: depth pre-pass plus screen-space sun-shadow collection into the colour target; at 0x4000 that is resolved to an `8_8_8_8` mask (`06ACD000`) and the colour is cleared | 0.79 | - |
| 0x2000/0x4000 | a few draws; the shadow-mask resolve | 0.18 | - |
| 0x10000 | vehicles: stencil ref = 4·k + {0,1}, blended through the 7e3 target's 64-bit (`_AS_16_16_16_16`) view | 0.51 | 1.75 |
| 0x20000, 0x40000, 0x80000 | world: opaque and alpha-tested | 1.73 | 2.84 |
| 0x100000-0x800000 | small translucent/glass sets, a depth-only set | 0.13 | 0.21 |
| 0x02000000 | depth resolved to `078B0000`, then soft particles, rain and light volumes that sample it | 0.19 | 0.26 |
| 0x04000000 | full-screen fog/atmosphere (reads depth and the 1x1 luminance); final resolves: depth and stencil → `06ACD000`, HDR colour → 1280x720 16x4F | 0.32 | 0.77 |
| 0x08000000 | post: 640x360 → 320x180 downsamples; log luminance 128 → 64 → 16 → 4 → 1x1 with adaptation (`02D6C000`); bright pass; motion blur at 640x360 16x4F (`sub_8260E9A0`); bloom blurs; 256x256 4x minimap/gauge render-to-texture | 0.98 | 1.03 |
| 0x10000000 | the tonemap (1 draw), then by night 85-100 additive light glows, coronas and beams | 0.42 | 0.60 |
| 0x20000000 | HUD and UI; 24 point draws with no target (occlusion queries for the glows) | 0.26 | 0.18 |

The main scene targets are:
- **colour:** `2_10_10_10_FLOAT` (7e3) with exponent bias 4, at EDRAM 1024, pitch 1280;
- **depth:** `D24FS8` at EDRAM 0;
- 2x MSAA, two tiles (bins 0x3 and 0xC).

Pause, GPS map and race pause render the world at 4x MSAA in three tiles
(1280x256, 256 and 208).

## 2. The upscaler-research questions

1. **Pass list and shares.** The phases are in §1. At 3x by day:
   - draws 5.7 ms (64%);
   - texture loads 1.1 ms (12%);
   - resolves 1.0 ms (11%);
   - uploads 0.66 ms (7%);
   - EDRAM transfers 0.33 ms (4%), 0.25 ms of which is binding the final 1x
     target at EDRAM 720 over the scene's colour range for the tonemap.
2. **Where the HUD starts.** At phase 0x20000000, in the same target as the
   tonemap: 1x `8_8_8_8` at EDRAM 720, pitch 1280, with a 1x `D24FS8` cleared by
   a 4x rectangle. The shaders:
   - `46BE7CEEBA3ECD76`: the Flash UI movies;
   - `7E42B2715495CD22`: the minimap composite, from a 256x256 target drawn in post;
   - `7EAAF462508AC81A` and `C1E1626A8C204162`: the speedometer, a 3D mesh of
     about 1,600 indices. It samples the environment map `032E2000` and a
     256x256 gauge face rendered in post.

   Everything is `SrcAlpha/InvSrcAlpha`, for colour and alpha alike.
3. **Mid-scene resolves sampled later in the scene: yes.**
   - By day, the screen-space sun-shadow mask (phase 0x4000, `8_8_8_8`) is read
     by about 520 draws per tile in 0x20000-0x100000. Their UVs come from the
     VS-projected position (e.g. `93307A3906A73EF9`: `xy/w`, then a fetch with
     a 0.5 offset).
   - Depth resolved at 0x02000000 is read by the soft particles and volumes of
     0x02000000 and the fog of 0x04000000: about 15 draws per tile by day,
     about 140 per tile in the race.
   - The final depth and colour resolves are read only by post, the glows and
     the HUD.
4. **Pixel position.** Only two shaders use param gen:
   - the vehicle shader `4A4E8AEEC989A814` in phase 0x10000: 24 draws in the
     city, 156 in the race;
   - `0683EBE095DBB616` in 0x100000: 8 race draws.

   Screen-derived UVs come from item 3 instead: the shadow mask and depth reads.
5. **Scene target.**
   - Format: 7e3, bias 4, with a 64-bit view for blended phases; `D24FS8`.
   - MSAA: 2x in play, 4x in menus.
   - Tiling: 2 tiles in play, 3 in menus, whatever `mcla_single_tile` is set
     to. `grcDevice::BeginTiledRendering` (`sub_8217A470`) does run right
     before phase 0x800, with the bin select inside it.
6. **Phase bits to passes.** §1.
7. **Depth and stencil at the end of the scene.** The vehicle ID is in the
   stencil: `ref = 4·k + {0,1}` with always/replace. k is 1 in the city and
   1 to 8 in the race; bit 0 is set on some draws of each car. Depth and
   stencil are resolved per tile at 0x04000000 to a `24_8_FLOAT` texture
   (`06ACD000` by tile), which the motion-blur shader reads as `8_8_8_8` to
   get the stencil.
8. **GenVelocity.** No velocity target in play. The motion blur
   `A6B9B6B0FA692C54` (in `sub_8260E9A0`, post phase) works at 640x360 and
   outputs 16x4F: blurred colour plus alpha as the blend weight (mean 0.3 in
   the race, about 0 standing). Per pixel it:
   - reads depth (tf2) and depth-as-`8_8_8_8` (tf1), which gives the stencil;
   - computes index = trunc(stencil·c250.x);
   - uses c64-c67 (reprojection) and `c[69+a0]..c[72+a0]` (per-vehicle rows);
   - takes 8 taps along the motion.

   Vehicles are covered; anything else that moves is not.
9. **3x against 2x, the same views** (GPU busy time, idle excluded): day 8.9
   against 6.9 ms, night 7.1 against 5.9, race 11.4 against 9.6.

## 3. HDR

**The tonemap pass.** It is the first draw of phase 0x10000000: PS
`E5EBEFAE4D165CBA`, VS `1FAB9787BCA32D7C`.

Inputs:
- t0: the HDR scene, 1280x720 `16_16_16_16` float, resolved from the 7e3
  target with bias −4, so in shader units;
- t1: 640x360 motion-blur colour and weight;
- t2: 320x180 bloom;
- v16: the 1x1 adapted luminance at `02D6C000`, which the VS clamps to
  [c64.x, c65.x] = [0.15, 0.2] (0.115-0.2 in the race).

It uses no LUT textures, and writes the 1x `8_8_8_8` at EDRAM 720 with no
blending.

Operator, decoded from the ucode:
1. c = lerp(scene, blur, sat(4·blur.a)).
2. XYZ = M·c, with M the DirectX SDK matrix (0.5141364, 0.3238786,
   0.16036376 / 0.265068, …). Then x, y and Y.
3. Ls = c64.z·Y / (Lavg + 0.001), with key c64.z = 0.25 by day and 0.2 at night.
4. Ld = Ls·(1 + Ls/W)/(1 + Ls), with W = c65.x = Lwhite².
5. Back to RGB through x and y, so hue is preserved.
6. Add the bloom.
7. Saturation around Rec.709 luma (c67.x).
8. Colour balance: 2·c68.xyz, then −0.5.
9. Contrast c69.x around 0.5.
10. Encode: `1.055·x^(1/2.2) − 0.055`, or `12.92·x` below 0.00304. Alpha = 1.

The constants follow the time of day:

| | key | W (Lwhite) | saturation | balance |
|---|---|---|---|---|
| Day | 0.25 | 0.335 (0.58) | 1.0 | (0.5, 0.5036, 0.5) |
| Dusk | 0.25 | 0.214 (0.46) | 1.0 | (0.5, 0.5003, 0.5) |
| Night city | 0.2 | 2.25 (1.5) | 0.9-1.0 | (0.496-0.5, 0.5, 0.5) |
| Night race | 0.2 | 2.25 (1.5) | 0.9 | neutral |
| Title | 0.25 | 0.21 | 0.9 | warm (0.58, 0.50, 0.42) |

**Invertible?** Below the white point, yes: the curve is a monotonic
quadratic. Above it, colour is clipped per channel in 8 bits. Bloom and glows
are added afterwards and everything is quantized to 8 bits, so the LDR output
cannot be inverted exactly. The inputs, though (the HDR texture, bloom, blur,
1x1 luminance and constants), are all there for a replacement HDR curve at
the same point.

**Range of the pre-tonemap scene.** Read back at native with
`readback_resolve=full`, shader units:

| Scene | Max channel | p99.9 | > 1.0 | White point in scene units | Clipped by the curve |
|---|---|---|---|---|---|
| City, 13:00 | 0.44 | 0.29 | 0% | 0.35 | 0% |
| City, 17:30 (sky) | 1.18 | 0.49 | 0% | 0.28 | 1.3% |
| City, 23:30 (neon, lamps) | 1.55 | 0.73 | 0.01% | 1.13 | 0% |
| Title, sun in view | 1.99 (sun Y 1.87) | 0.59-0.82 | 0.01-0.08% | 0.28 | 0.15% |
| Night race, rain (headlights, checkpoint chevrons) | 1.99 | 0.75-0.99 | 0.02-0.09% | 0.87 | 0.02-0.09% |

1.992 is 31.875·2⁻⁴, the 7e3 ceiling. The brightest pixels reach 1.3x (day)
to 6.7x (sun) the tonemapper's white, at most about 2.7 stops. The bloom
(up to 0.98 on the title) and the night glows add the rest after the curve,
in display space.

**Transfer of the final image.**
- The game writes gamma-encoded values (the curve above) into a plain
  `8_8_8_8` target. The target is not `_GAMMA`, and no PWL or `_AS_16` format
  is involved.
- At the swap, fetch 0 is format 6 (`8_8_8_8`) and `DC_LUTA_CONTROL` = 0. The
  presenter applies the 256-entry, 10-bit table, the same in every mode
  captured.
- The XDK helper `sub_82426B68` builds that table from
  `VdGetCurrentDisplayGamma`, which the SDK answers with type 2 (TV/BT.709)
  and power 2.222. The table is BT.709 OETF(sRGB EOTF(x)): within 1 code at
  64, 128 and 192 (193, 462, 741 out of 1023). Type 1 (sRGB) would give an
  identity table, so this darkening of the shadows is the SDK's choice of TV
  output.
- There is no brightness or gamma option in the game (Options: Controller,
  Game, Audio, Camera; Game: subtitles, auto save, arrow, steering, convertible
  top; Camera: camera, cinematic camera, shake).

**After the tonemap, before the HUD.** No AA, grain, vignette or LUT. The
grading lives inside the tonemap shader. What follows the tonemap in phase
0x10000000, additive (`One/One`) into the LDR target and depth-tested by
sampling `078B0000` at the projected light position:
- light glows `085237B6D0D5ED17` (one 4-vertex strip per light; 83-96 at
  night);
- coronas `16CE3D9FC3877FA6` (a quad list);
- a beam/flare mesh `F7E0610D4F088236` with a gamma DXT1 texture.

By day there are none.

**HUD and full-screen UI.**
- **Gameplay and replay:** the HUD draws into the tonemapped target, as above.
  It includes 3D (the speedometer mesh) and render-to-texture (minimap, gauge
  face).
- **Pause, race pause, GPS map, replay confirmation:** the world is still
  rendered, at 4x, and tonemapped into the 1x target, which is resolved to a
  texture (`07FE0000`). The UI phase then runs predicated-tiled in a 4x
  `8_8_8_8` at EDRAM 1024. Its first draw copies that texture (opaque
  `46BE…`), then the menu panel (a 960x640 2x MSAA texture rendered in post)
  and the UI follow.
- **Loading map zoom-out and GPS map:** a 3D scene through the full phase loop
  and the tonemap.
- **Legal screens:** no phase loop; UI only, into the 1x target.
- **Bink** (attract movie): the phase loop runs with an almost empty scene,
  then after it, still inside `sub_822C0980`, the movie is drawn with PS
  `5F1C9EC7A520C5F1`: BT.601 limited-range YUV to RGB from three 8-bit planes,
  alpha-blended into the final target. That is video-gamma RGB, never
  linearized, so it should be treated as SDR UI.
- **Garage, photo mode and cutscenes** were not reached.

**Boundary hook candidates.**

| Candidate | Where | Robustness |
|---|---|---|
| Phase markers (game-side NOP from the `sub_822E55D0` override, with the loop's enter/leave) | scene ends at 0x08000000; HDR inputs ready at 0x10000000; HUD starts at 0x20000000; after the loop leaves = UI (Bink) | Seen in every 3D mode captured (play, title, loading, pause, GPS map, replay, attract). Legal screens have no loop, which is detectable. In the menus' tiled UI phase the 0x20000000 and leave markers are replayed per tile (3x), so they must be treated as idempotent. Safe only from the render thread and at this level: the per-state-call marker corrupted the stream. Two function overrides, no codegen change. |
| PM4 marker of another kind (e.g. a register write) | same | No advantage over the NOP; the SDK already parses NOPs. |
| Shader hash | the tonemap PS `E5EBEFAE4D165CBA`, the first draw of the tonemap phase in every capture | Good fallback for "tonemap here". The HUD start is not hash-detectable: the glows come between, and `46BE…` is also used for render-to-texture in post. |

## 4. Upscaler inputs

**Depth at the end of the scene.**
- In EDRAM at base 0, `D24FS8` (20e4 float plus stencil), 2x MSAA. Both
  tiles go to the same EDRAM, so no host render target ever holds the whole
  frame.
- As textures, per tile: `078B0000`, resolved at 0x02000000 before the last
  particles and fog; and `06ACD000`, resolved at 0x04000000 with the stencil.

**Velocity.**
- None stored (see §2.8). Full-resolution vectors need a pass that repeats the
  blur's reprojection: depth, stencil k, camera reprojection and vehicle rows.
- Those constants reach the GPU only in the post phase, after the upscale
  point, so they must be captured game-side.
- Pedestrians, wheels, particles and traffic without an ID would ghost.

**Camera and jitter.**
- Rage's per-object VS constants: c0-c3 World, c4-c7 WorldView, c8-c11
  WorldViewProj (13-21 distinct values per frame), c12-c15 ViewInverse (c15
  is the camera position, shared).
- No single projection register exists, so jitter has to be the SDK's per-draw
  ndc_offset on draws between the 0x800 and 0x08000000 markers.
- Screen-space draws must be left alone: clear rectangles, the 0x04000000 fog,
  and draws whose VTE skips the viewport transform.
- Sub-pixel jitter misregisters the VS-projected shadow-mask lookups by the
  jitter amount. That is harmless; a sub-rectangle render is not (see §6).

**Pixel position.** §2.4, two shaders.

## 5. Pass costs and what an upscale could save

GPU busy time per group, 1x / 2x / 3x, in ms. Quiet frames, one submission
per frame, intervals over 0.5 ms counted as idle.

| | pre (0x1-0x400) | scene (0x800-0x04000000) | post | tonemap + glows | HUD | sum |
|---|---|---|---|---|---|---|
| City day | 2.09 / 2.59 / 3.19 | 2.61 / 3.21 / 3.90 | 0.38 / 0.63 / 0.98 | 0.07 / 0.23 / 0.42 | 0.22 / 0.23 / 0.24 | 5.4 / 6.9 / 8.9 |
| City night | 1.15 / 1.60 / 1.90 | 2.47 / 3.22 / 3.19 | 0.30 / 0.56 / 0.93 | 0.11 / 0.32 / 0.64 | 0.10 / 0.24 / 0.26 | 4.1 / 5.9 / 7.1 |
| Race | 1.68 / 2.11 / 2.74 | 5.76 / 6.32 / 6.66 | 0.39 / 0.60 / 1.03 | 0.25 / 0.37 / 0.60 | 0.11 / 0.20 / 0.18 | 8.2 / 9.6 / 11.4 |

The scene's cost hardly grows with resolution. A linear fit over the day
city's pixel count gives about 2.45 ms fixed and about 1.45 ms of pixel work
at 3x. Draw counts do not change with scale (about 2,700 a frame, half per
tile), so the fixed part is per draw.

A scene-only upscale keeps pre, post and HUD at 3x. Rendering the scene at
2/3 (equivalent to 2x) saves 0.69 ms by day, about 0 at night and 0.34 ms in
the race; at 1/2, roughly 1.1, 0.4 and 0.7 ms. That is before the upscaler's
own cost at 4K (other agent's research), so the 3x frame stays near 9 ms by
day, 7 at night and 11.5 in the race: about 87-140 FPS, against the game
thread's cap of about 6.8 ms (147 FPS).

Larger levers that showed up:
- **The two-tile replay.** Every scene draw runs twice; making
  `mcla_single_tile` work could save part of the 2.5-5.5 ms per-draw part.
  This is an estimate, not measured.
- **2x MSAA off:** −18 to −25% in earlier measurements.
- **The 0.25 ms transfer** when the tonemap binds the final target.

## 6. Unknowns and risks found

- Not captured: garage, photo mode, cutscenes; phases 0x1, 0x10, 0x40000,
  0x100000 and 0x800000 are only partly identified.
- Why `mcla_single_tile` has no effect. The hook site runs inside
  `sub_8217A470`, but the bins stay 2. Answered in `single-tile-2026-10-16.md`:
  no call passes a null render target, and one 2x tile needs 2,944 EDRAM
  tiles against the SDK's 2,048.
- **A sub-rectangle (scene at reduced size) upscale has to handle:**
  - two tiles with window offsets and per-tile resolves;
  - the day shadow mask and depth resolves read through VS-projected UVs
    (they would need remapping or an upscale at each resolve);
  - param-gen `vPos` in the vehicle shader.

  A DLAA-style temporal pass at full 3x avoids all three.
- DoF: the blur weight was about 0 standing still. Whether cinematic replay
  cameras add DoF through the same buffer was not checked.

## Reproducing

```
cd third_party\rexglue-sdk
git apply ..\..\launcher\dev\gpu-timestamps.sdk.patch
git apply ..\..\launcher\dev\frame-log.sdk.patch
cd ..\..
git apply launcher\dev\render-markers.patch
```

Then:
1. Rebuild the SDK (`cmake --build` and `--install` in the SDK's
   `out\build\win-amd64`) and the game (`scripts\build.ps1 -NoCodegen`); copy
   `rexgpu-xenos.dll`.
2. Run with `devrun.py run --script <pad> --res 3 --extra
   --tmp_frame_log_dir=<dir> --mcla_dev_render_markers=true
   [--dump_shaders=<dir>] [--d3d12_threaded_submission_split_draws=0]`.
3. In the pad script: `set tmp_frame_log_quiet true|false`, then
   `set tmp_frame_log <label>`.
4. Reverse the three patches and run `build-game.ps1 -ResetSdk`.

# Single-tile rendering in a 4,096-tile EDRAM (2026-10-16)

`single-tile-2026-10-16.md` found that `mcla_single_tile` never took effect: its
hook waited for a null render target that no call passes, and the SDK's EDRAM
is the console's 2,048 tiles, too few for a 720p 2x MSAA scene in one tile
(1,472 tiles of colour and 1,472 of depth). This note covers the fix: an EDRAM
of 4,096 tiles as an SDK option, the hook aimed at the main scene, the checks,
and what one tile is worth now that it works. Windows, RTX 5080, Ryzen 7
9800X3D, launcher settings.

## Summary

- **SDK:** `patches/rexglue-edram-size.patch` adds `edram_tiles` (2048, the
  console's and the default, or 4096; read at start-up). With 4096 the EDRAM
  wraps at 4,096 tiles, bases are 12 bits, the EDRAM buffer is twice the size
  (180 MiB at 3x) and the 22 EDRAM resolve shaders per API have copies that
  wrap at 4,096. At 2048 two changes apply too (a resolve clear across the end
  of the EDRAM is no longer dropped, and 9-bit pitches in the transfer
  shaders); neither shows in this game.
- **Game:** the default is `edram_tiles = 4096` on PC and on the PS5 (untested
  there). The hook acts on any tiled pass whose colour and depth fit in the
  EDRAM the GPU emulates: the main scene with 2x MSAA becomes one 1280x720
  tile, one bin and one resolve per target. The 4x passes (pause menu, GPS map)
  don't fit and keep three tiles. A change of anti-aliasing between one-tile
  frames goes through 8 frames in two tiles, and one tile never takes the last
  free slots of the game's tile surface cache. The pause menu's PERFORMANCE ›
  SINGLE TILE RENDERING row is back and switches during play; it says
  "(UNAVAILABLE)" with 2,048 tiles, and doesn't change then. The launcher has
  *Single tile rendering*.
- **Checks:** resolves through the new shaders compared byte for byte with
  the direct path (33,700, none different); the picture with one tile against
  two within one run, in every scene, at 1x, 2x and 3x on both APIs, and on
  the ROV, FSI and EDRAM buffer paths; nothing in the game crosses tile 2,048
  except the single tile's own colour, and with 2,048 tiles nothing wraps at
  all; switching during play, 50 times each way, from the pause menu and
  without it (the F4 overlay's way); the game's tile surface cache at 5 of 8
  slots at most; the D3D12 debug layer and Vulkan synchronization validation
  report nothing new; on PC, the PS5 host's settings change nothing.
- **Gains** (anti-aliasing on, one tile against two, in-run alternation):
  - 3x: frames 9 to 11% shorter in the city by day, 15 to 20% in the race;
  - native: the race 14% shorter on D3D12 and 25% on Vulkan, where the GPU
    command thread was the bottleneck; the city stays at the game's own limit;
  - the GPU command thread's CPU time 28 to 35% lower;
  - GPU time 1.0 ms lower at 3x in the city and 1.1 to 2.2 ms in the race;
  - with a 120 FPS cap at 3x in the city, frames within 1 ms of the target
    93-94% → 99%.
- **Anti-aliasing off** is now 2 to 5% faster than on at 3x (it was 8 to 18%
  faster than two tiles).
- **Not verified:** the PS5 (its build sets 4,096 tiles now, untested on the
  console), Linux, and the garage.

## What changed

### SDK: `patches/rexglue-edram-size.patch`

- **`edram_tiles`** (GPU, 2048 or 4096, restart): the size of the EDRAM the
  render target cache emulates. 2048 is the console's and the default. With
  4096, EDRAM addresses wrap at 4,096
  tiles instead of 2,048 and render target bases are 12 bits. The cache asks
  each backend for the largest buffer it can bind whole and falls back to
  2,048 tiles, with a warning, if the larger EDRAM's buffer would not fit
  (D3D12: 2^27 32-bit elements, 512 MiB; Vulkan: `maxStorageBufferRange`).
  The buffer is 20 MiB at 1x, 80 MiB at 2x and 180 MiB at 3x (twice the
  console size).
- **Constants and registers.** `xenos::kEdramTileCount` and
  `kEdramSizeBytes` are gone, so every use had to choose:
  `kEdramTileCountConsole` (2,048), `kEdramTileCountMax` (4,096),
  `GetEdramSizeBytes(count)`. `RB_COLOR_INFO` and `RB_DEPTH_INFO` give their
  base through `GetBaseTiles(edram_tile_count)`, which keeps 11 bits with the
  console's EDRAM and 12 with the larger one. `RenderTargetKey` has a 12-bit
  base, as do Vulkan's framebuffer keys.
- **Render target cache.** `edram_tile_count()` is the size in effect. The
  ownership map, its wrap-around, the transfer rectangles and resolve areas use
  it. A host render target stays as tall as the console's EDRAM from its base
  (`GetRenderTargetHeight`, `GetRenderTargetExtentTiles`), so host render
  targets don't get bigger. The transfer shaders' address constant packs the
  two pitches in 9 bits each instead of 10 (a render target's pitch is at most
  255 tiles at 32bpp, 510 at 64bpp), so that the difference of two 12-bit
  bases fits beside them.
- **At 2,048 tiles too,** two of these change what runs, though nothing in
  this game reaches them (with 2,048 tiles nothing crosses tile 2,048 at all,
  `edram-wrap-log.sdk.patch`):
  - a resolve clear whose area wraps round the end of the EDRAM was dropped:
    `PrepareHostRenderTargetsResolveClear` subtracted the render target's base
    from the resolve's without the wrap-around, so the offset underflowed. It
    is now taken modulo the EDRAM size. The same bug is in xenia;
  - the transfer shaders are generated with the 9-bit pitches above.

  The PS5 runs both, untested.
- **Resolve shaders.** The 22 EDRAM buffer resolve shaders per API (fast, full
  and clear, unscaled and scaled) wrap at the console's 10 MiB and read an
  11-bit base. Copies with a 4,096-tile period
  (`src/graphics/shaders/resolve_edram4096.xesli` and 22 `*_edram4096.cs.xesl`
  entry files) are built from the same xenia 0b2ffa3 sources with two functions
  redirected: `XeEdramOffsetInts` wraps at 4,096 tiles, and `XeResolveGetInfo`
  also takes base bit 11, which the host passes in bit 30 of
  `xe_resolve_edram_info` (`ResolveEdramInfo::base_tiles_bit_11`; bits 0 to 29
  are as before). Rebuilt with the same fxc and glslangValidator as the fork's
  other shaders; the original 22, rebuilt the same way, came out identical to
  the SDK's. With the larger EDRAM their pipelines are created instead of the console
  ones.
  The direct resolve shaders read render targets, not the EDRAM buffer, and
  are unchanged.
- **ROV and FSI.** The translators get the EDRAM size from the pipeline caches
  and wrap the pixel shaders' EDRAM addresses at it; the command processors
  pass 12-bit bases.
- **For the title.** `IGraphicsSystem::edram_tile_count()` (0 until the GPU
  has started), set by the command processors once the render target cache
  has chosen. That changes the interface, so `kGpuPluginAbiVersion` is 2.

Hand-written: about 680 lines added and 210 removed, over 22 source files and
the 23 shader sources; the rest is the 44 compiled shader headers.

### Game

- **The hook** (`mcla_single_tile`, 0x8217A700 in `BeginTiledRendering`,
  `src/render_perf.cpp`) now takes r18, the renderer's MSAA mode, instead of
  r17. It acts when the game has computed more than one tile and the whole
  target's colour and depth fit in the EDRAM the GPU emulates
  (`IGraphicsSystem::edram_tile_count`): 2 x 1,472 tiles for 720p with 2x
  MSAA. Then the tile is the target's size and the count 1, and
  `EndTiledRendering` does one bin and one resolve per target. 4x MSAA (the
  pause menu, the GPS map) needs 2 x 2,944 and keeps its three tiles; without
  MSAA the game draws one tile by itself. With the console's 2,048 tiles the
  hook changes nothing. It also takes r19, the target's format, the last part
  of the tile surface cache's key (below), and checks each surface against
  2,048 tiles as well as both against the EDRAM size, in case the SDK's
  maximum ever grows.
- **Anti-aliasing changes between one-tile frames** go through frames in two
  tiles (`mcla_single_tile` and `perf_options.cpp`'s `ApplyMsaa`; "Changing
  anti-aliasing between one-tile frames" below).
- **The tile surface cache:** the hook logs how many of its 8 slots are taken
  each time that grows, and doesn't make one tile when its key isn't cached
  yet and 7 or more are taken ("The tile surface cache" below).
- **`mcla_edram_limit`** applies only where the GPU emulates more than 2,048
  tiles; the single tile's surfaces don't come through it.
- **The PC default** `edram_tiles = 4096` (`MclaApp::ApplyPcDefaults`). The
  defaults are now applied in `OnPreSetup`, once the GPU plugin is loaded (also
  for `mcla_gpu_backend = any`, which used to leave the loading to the SDK), and
  before the GPU starts, which reads `edram_tiles`. A line in `mcla.toml` or an
  option on the command line replaces it, as for the other defaults. When the
  chosen backend fails to load, the plugin's default backend is loaded there
  too (as the SDK would do next), so the defaults still apply.
- **The pause menu's PERFORMANCE › SINGLE TILE RENDERING row** is back, first
  in the list, and switches during play: the game reads the tile count every
  frame. It says "(UNAVAILABLE)" where the GPU emulates only 2,048 tiles
  (`edram_tiles = 2048`, or a GPU that can't bind the larger buffer), and A
  leaves it as it is then, so that such a session doesn't save `false` for
  the next one.
- **The PS5** (`ps5/game/main_ps5.cpp`) sets `edram_tiles = 4096` as a default
  too, before it creates the graphics system (`MCLA_EDRAM_TILES`; an
  `edram_tiles` line in `/data/mcla/mcla.toml` overrides it, and
  `-DMCLA_EDRAM_TILES=2048` builds it with the console's size). Its graphics
  system is linked in, from the same headers, so the plugin ABI version and
  the option's registration are as on PC; RADV's `maxStorageBufferRange` is
  4,294,967,295. Untested on the console; see "The PS5" below.

### Launcher

*Single tile rendering*, On or Off, under anti-aliasing (`mcla_single_tile` in
`mcla.toml`). It does nothing with anti-aliasing off; `edram_tiles = 2048` in
`mcla.toml` goes back to the console's EDRAM altogether.

## Checks

All with `edram_tiles = 4096` (the PC default) unless said otherwise.

- **Start-up.** The log says "PC defaults: edram_tiles = 4096" and
  "RenderTargetCache: emulating an EDRAM of 4096 tiles"; with
  `--edram_tiles=2048`, "edram_tiles = 2048 set by the user" and nothing about
  the EDRAM.
- **Frame structure** (D3D12 frame log at 1x). One tile: one bin
  (`select` 0x3) and one 1280x720 resolve per target, the scene's colour at
  EDRAM 1,472 (`RB_COLOR_INFO` 004305C0) and its depth at 0. Two tiles: bins
  0x3 and 0xC and two resolves per target (1280x512 and 1280x208), colour at
  1,024. Draws a frame at 1x in the city: 3,119 with one tile, 4,517 with two,
  3,172 with anti-aliasing off.
- **Resolves, byte for byte.** `direct-resolve-validate.sdk.patch` does each
  direct resolve also through the EDRAM buffer, which with 4,096 tiles uses
  the new shaders, and compares the two: the one-tile scene's colour and depth
  resolves cross tile 2,048 there. 33,700 resolves: the city at 3x on both
  APIs (6,150 on D3D12, 7,300 on Vulkan), the race at 3x (3,500 and 4,200) and
  the city at native on D3D12 (12,550). None differed.
- **What crosses tile 2,048** (`edram-wrap-log.sdk.patch`). With 4,096 tiles:
  the one-tile scene's colour, tiles 1,472 to 2,912 (as a 2x target and as the
  4x clear rectangle over it), and its resolve; nothing else. With 2,048 tiles,
  through the title, the map zoom-out, the city by day and at night, the pause
  menu and the GPS map: nothing crosses, so nothing in the game relies on the
  console's wrap-around there, and the larger EDRAM changes nothing but the
  single tile (nor do the patch's two changes that apply at 2,048 tiles).
- **The picture.** One tile against two, switched within a run with game
  time almost stopped (`mcla_fixed_dt 1000`), shots on consecutive frames (one,
  one, two, two, one): the title, the map zoom-out, the city by day and at
  night, the pause menu and the GPS map. Compared pixel by pixel, the
  difference across a switch is the same as between two frames in the same
  mode, under 0.5 of 255 on average, and there is no seam at the old tile
  boundary (row 512, 1,536 at 3x). Done at 1x, 2x and 3x on D3D12 and Vulkan;
  through the EDRAM buffer path (`--direct_host_resolve=false`) on D3D12 at 3x
  and Vulkan at 1x; on ROV (`--render_target_path_d3d12=rov`) and FSI
  (`--render_target_path_vulkan=fsi`) at 1x and 3x. The race, its pause menu
  and its replay (time not stopped) look the same either way. The one
  exception, ROV at 1x at night after switches without the menu, is under
  "Changing anti-aliasing between one-tile frames".
- **Switching during play** (`city_switch` pad; draws a frame per segment
  tell one tile from two):
  - 50 switches of `mcla_single_tile` 12 frames apart: the draws go between
    one tile (about 3,100) and two (about 4,500) every time;
  - 50 switches of `mcla_msaa`: one tile throughout (anti-aliasing off is one
    tile by itself);
  - all four combinations: one tile with anti-aliasing on and the option on,
    two tiles with it off, one tile with anti-aliasing off whatever the option;
  - five rounds of switches just before and while the pause menu is open, and
    switches with the GPS map open;
  - the pause menu's row: A on SINGLE TILE RENDERING unticks it, and once the
    menu is closed the scene is in two tiles (D3D12 at 1x: 4,514 draws a
    frame, from 3,118); at night afterwards, one tile and two look the same;
  - the race: 20 switches of each during the race, the race's pause menu and
    the replay;
  - after the fix for anti-aliasing changes between one-tile frames, the
    vehicle pass's MSAA mode in frame logs (below).
- **Video memory** (`nvidia-smi`, at each segment): flat through the 50
  switches of each kind. It rose once when the other mode's targets were
  first made: two tiles 42 MB at 1x and 360 MB at 3x (376 MB in the race),
  anti-aliasing off 20 and 106 MB (364 MB in the race at 3x); the larger
  EDRAM buffer itself is 10 MiB more at 1x, 90 MiB at 3x.
- **D3D12 debug layer** (`--d3d12_debug=true`), the switching pads in the city
  at 1x and 3x and in the race at 3x: no errors or corruption; the warnings are
  the usual ones (initial states ignored for buffers, MIN/MAX blend factors,
  live objects at exit).
- **Vulkan validation with synchronization validation**
  (`VK_LAYER_VALIDATE_SYNC=1`, `VK_LAYER_SYNCVAL_SHADER_ACCESSES_HEURISTIC=1`,
  `VK_LAYER_ENABLE_MESSAGE_LIMIT=0`), the same pads in the city at 1x and 3x
  and in the race at 1x: only write-after-write hazards between compute
  dispatches, into the texture loads' scratch buffer or into the EDRAM buffer
  (its clears and dumps), the kinds `direct-resolve-2026-10-15.md` found before.
  The same pad with 2,048 tiles gives the same kinds in about the same numbers
  (39,018 against 37,635 in the city at 1x; 42,345 at 3x, 49,086 in the race,
  which ran longer). The logs rotated, so the title's part wasn't kept.
- **The console's size** (`--edram_tiles=2048`): two tiles whatever
  `mcla_single_tile` says (50 switches: always about 4,500 draws), anti-aliasing
  off still one tile, and the pause menu's row reads SINGLE TILE RENDERING
  (UNAVAILABLE).
- **Smoke tests of the final build,** both APIs at native and 3x: one tile
  (3,110 to 3,156 draws a frame in the city) and two (4,476 to 4,500); A on the
  pause menu's row, then two tiles once the menu closed (4,494 to 4,541); one
  tile and two at night.

## Measurements

Each run alternates the three modes within one session, so all of them see
the same scene:
- **one tile:** anti-aliasing on, `mcla_single_tile` on (the default now);
- **two tiles:** anti-aliasing on, `mcla_single_tile` off (as before);
- **anti-aliasing off:** one tile, as the game draws it by itself.

After each switch the pad opens and closes the pause menu, as a player
changing the setting would (see "Switching without the pause menu" below), and
the segment starts after that.
- **City:** standing at the spawn point at 13:00 (time of day poked),
  uncapped; four rounds of about 8 s per mode.
- **Race:** the race benchmark (Sunset and Vine, midnight, rain, 7 opponents,
  timedemo at 60 updates a second, uncapped); three rounds of 600 updates per
  mode.
- **Frame:** the median swap interval without each segment's first 40 swaps,
  and the 1% low.
- **GPU:** D3D12 timestamped frames (`frame-log.sdk.patch` on
  `gpu-timestamps.sdk.patch`, with the development `render-markers.patch`,
  since replaced by `src/render_markers.cpp`), 18 to 24 a mode,
  idle gaps over 0.5 ms left out; the scene is phases 0x800 to 0x04000000.
  Vulkan: whole-frame timestamps (`gpu-timestamps-vulkan.sdk.patch`), one a
  second. Both include the timestamps' own cost.
- **GPU command thread:** its CPU time per swap (`GetThreadTimes`, no SDK
  patch).

Frame time, median (1% low), two tiles → one tile, and anti-aliasing off:

| | Two tiles | One tile | Anti-aliasing off |
|---|---|---|---|
| City, D3D12 3x | 8.00 ms (105 FPS) | 7.12 ms, −11.0% (118 FPS) | 6.97 ms (116 FPS) |
| City, Vulkan 3x | 8.07 ms (102 FPS) | 7.33 ms, −9.2% (115 FPS) | 7.10 ms (118 FPS) |
| City, D3D12 native | 6.97 ms | 7.03 ms | 7.02 ms |
| City, Vulkan native | 6.96 ms | 6.97 ms | 6.98 ms |
| Race, D3D12 3x | 10.06 ms (70 FPS) | 8.08 ms, −19.7% (94 FPS) | 7.86 ms (93 FPS) |
| Race, Vulkan 3x | 10.87 ms (59 FPS) | 9.28 ms, −14.6% (83 FPS) | 8.79 ms (83 FPS) |
| Race, D3D12 native | 8.70 ms (76 FPS) | 7.48 ms, −14.0% (92 FPS) | 7.28 ms (106 FPS) |
| Race, Vulkan native | 10.00 ms (63 FPS) | 7.47 ms, −25.3% (100 FPS) | 7.28 ms (100 FPS) |

Draws, GPU and the GPU command thread, two tiles → one tile (anti-aliasing off
in brackets):

| | Draws | GPU busy | Scene GPU | GPU command thread |
|---|---|---|---|---|
| City, D3D12 3x | 4,568 → 3,167 | 8.56 → 7.50 ms (7.26) | 3.71 → 2.66 ms (2.34) | 6.04 → 4.02 ms, −33% (4.19) |
| City, Vulkan 3x | 4,565 → 3,180 | 8.62 → 7.66 ms (7.42) | | 6.77 → 4.89 ms, −28% (4.87) |
| City, D3D12 native | 4,605 → 3,190 | 5.32 → 4.32 ms (4.33) | 2.64 → 1.56 ms (1.47) | 5.52 → 3.99 ms, −28% (4.03) |
| City, Vulkan native | 4,621 → 3,226 | 5.00 → 4.07 ms (4.16) | | 6.24 → 4.32 ms, −31% (4.36) |
| Race, D3D12 3x | 5,930 → 3,867 | 11.00 → 8.82 ms (8.25) | 5.97 → 3.79 ms (3.48) | 9.47 → 6.11 ms, −35% (5.57) |
| Race, Vulkan 3x | 5,997 → 3,774 | 10.99 → 9.94 ms (8.79) | | 10.87 → 7.08 ms, −35% (6.77) |
| Race, D3D12 native | 5,898 → 3,821 | 7.54 → 5.95 ms (5.98) | 4.74 → 3.14 ms (3.19) | 8.50 → 5.88 ms, −31%; 94 → 76% busy (5.61) |
| Race, Vulkan native | 5,787 → 3,780 | 7.57 → 5.83 ms (5.79) | | 10.01 → 6.61 ms, −34%; 94 → 86% busy (6.60) |

How to read it:
- **The estimate held.** `single-tile-2026-10-16.md` expected one tile to
  save 0.8 ms of GPU time at 3x in the city and 1.3 ms in the race, and to
  keep about three quarters of anti-aliasing off's GPU saving. Measured on
  D3D12: 1.05 ms of scene time in the city (77% of what anti-aliasing off
  saves) and 2.18 ms in the race (88%).
- **The frame's limits.** In the city at native, the frame is the game's own
  limit (about 7.0 ms, 143 FPS) in every mode; the gain there is GPU time and
  CPU time on the GPU command thread. In the race at native that thread is
  the bottleneck with two tiles (94% busy), so one tile shortens the frame,
  most on Vulkan.
- **Anti-aliasing off against one tile:** 2 to 5% shorter frames at 3x (city
  2.1% on D3D12 and 3.1% on Vulkan, race 2.7% and 5.3%), about the same CPU
  time on the GPU command thread.

**With a 120 FPS cap at 3x** (the same pads without timestamps, real time;
the race in real time draws more than the timedemo segment), frames within
1 ms of 8.33 ms:

| | Two tiles | One tile | Anti-aliasing off |
|---|---|---|---|
| City, D3D12 | 92.8% (1% low 103 FPS) | 99.2% (109 FPS) | 98.8% (108 FPS) |
| City, Vulkan | 94.4% (102 FPS) | 99.0% (110 FPS) | 98.6% (109 FPS) |
| Race, D3D12 | 1.4%, median 12.00 ms | 36.4%, median 9.96 ms | 68.3%, median 8.78 ms |
| Race, Vulkan | 2.9%, median 13.04 ms | 25.8%, median 10.01 ms | 52.0%, median 9.26 ms |

The race at 3x doesn't hold 120 FPS in any mode on this machine; one tile
takes it from 77 to 83 FPS to about 100.

**The race benchmark** (`scripts\race.pad`: 90 s of the race at 60 updates a
second, uncapped) with the release build, no development patches, single tile
on and off in separate runs, two runs each, 3x, median frame time:

| | Two tiles | One tile |
|---|---|---|
| D3D12 | 10.12, 9.72 ms | 8.47, 8.12 ms (−16%) |
| Vulkan | 11.10, 10.92 ms | 9.32, 9.61 ms (−14%) |

The README's earlier 9.3 and 10.8 ms (two tiles) came from runs on
2026-10-15 (`direct-resolve-2026-10-15.md`); the benchmark's runs spread by 1 to
8% (that note), and today's two-tile runs are 1 to 9% above them. The
alternating runs above measure a later part of the race, with the pause menu
opened at each switch, so their figures differ from the benchmark's.

## Changing anti-aliasing between one-tile frames

**What happened.** The first round of measurements switched modes with the
pad's `set` alone, and some segments then carried EDRAM ownership transfers in
the scene every frame: 0.65 ms at 3x in the city with anti-aliasing off, 1 to
3 ms in the race, with one tile and with anti-aliasing off, never with two
tiles. In the frame logs, the vehicle pass (phase 0x10000) and what is drawn
after it set `RB_SURFACE_INFO` with the other MSAA mode than the scene's: 1x
draws in the 2x one-tile scene, or 2x draws in the 1x scene. The host render
target cache moved the scene between 2x and 1x targets for them.

**When.** The MSAA mode changed between two frames that were both one tile:
anti-aliasing off (which the game draws as one tile itself) and anti-aliasing
on with single tile. On the console the MSAA mode never changes, and one tile
only comes without MSAA. In an interactive session through the dev pad's pipe:
- two tiles → anti-aliasing off → single tile on → anti-aliasing on: the
  vehicles stayed 1x in the 2x scene, for at least 16 s;
- one tile → anti-aliasing off: 2x vehicle draws in the 1x scene;
- single tile off for a frame or more, then on again, or opening and closing
  the pause menu (three 4x tiles): back to normal.

The vehicles' surface state isn't held in the surface headers (their
`RB_SURFACE_INFO` and `RB_COLOR_INFO` words were right in the stale state);
where the game keeps it was not found.

**Who could reach it.** Not only the dev pad: the SDK's F4 settings overlay and
the console's `set` change `mcla_msaa` and `mcla_single_tile` during play
without the pause menu. Changes made inside the pause menu were clean,
including two tiles → anti-aliasing off → single tile on → anti-aliasing on in
one menu session: the menu's own tiled passes come in between.

**The fix** (`src/render_perf.cpp`, `src/perf_options.cpp`): a change of MSAA
mode between one-tile frames now has frames in two tiles around it.
- After a one-tile pass in another MSAA mode (anti-aliasing turned on with
  single tile on), the hook draws the next 8 passes in two tiles.
- Anti-aliasing off, while single tile is in effect, waits at the swap
  (`ApplyMsaa`) until 8 passes have been drawn in two tiles (up to 60 swaps,
  for a loading screen with no tiled pass).
- One frame in two tiles was not enough, nor three; four and more were. 8
  costs about 0.13 s of two-tile frames at 60 FPS, only when anti-aliasing
  changes.

**Checked** with frame logs (the vehicle pass's MSAA against the scene's, and
the transfers by phase), D3D12:
- the sequence above, anti-aliasing off and on from one tile, an off/on flip
  3 frames apart, and a mix with single tile switched while anti-aliasing was
  off, three times each per run: at native capped at 60 and uncapped, and at
  3x: no frame with the vehicles in the other mode (48 logged frames);
- the same through the pause menu: PERFORMANCE › SINGLE TILE RENDERING and
  ANTI ALIASING toggled with A in one menu session (two tiles → anti-aliasing
  off → single tile on → anti-aliasing on): clean, no scene transfers;
- started with anti-aliasing off (`mcla_msaa = false`) and turned on in the
  pause menu: clean, no scene transfers, the same transfers outside the scene
  as a session started with it on, with single tile on or off. (The game's
  "Shadow Collector" and "Depth Buffer" targets stay 1x then, as
  KNOWN-ISSUES.md describes for the anti-aliasing switch.)

The guest commands are the same on Vulkan; the checks above were on D3D12,
whose frame log shows them.

The ROV observation below came from switches of the tiles only, with
anti-aliasing on throughout, so it is not this.

The ROV path (`--render_target_path_d3d12=rov`, the default on Intel GPUs on
D3D12) has no ownership transfers, so such draws land in the EDRAM as they
would on the console. At 1x, after a series of switches without the menu at
frozen time, the night scene with two tiles showed foliage drawn black and a
patch of bushes over the car until a slow frame. The same sequence with
anti-aliasing switched instead of the tiles, with 2,048 tiles (so without the
single tile), left the bushes missing in one of two runs. It was not seen on
the default path, on FSI, at 3x, or with switches that went through the menu.
With the final build it came back in a run of that sequence and not in the
next: 6 of 8 runs in all. Anti-aliasing stays on in that sequence, so the fix
above doesn't touch it; the cause was not found.

## The tile surface cache

`BeginTiledRendering` (`sub_8217A470`) keeps the surfaces it tiles into in 8
slots: a colour surface pointer at 0x828393D8 + 4i (0 when free), its depth
32 bytes on, and its MSAA mode at 0x827D4284 + 4i; a surface has its width and
height at +56 and +58 and its format at +60. A key is MSAA mode, size and
format. Slots are never freed, and a new key with all 8 taken ends in
`sub_821BD618`, which loops for ever. One tile adds a key (2x, 1280x736).

The hook logs the count each time it grows. Every session measured reached 5
at most: 2x 1280x736 (one tile), two 4x 1280x256 (the menus, two formats),
2x 1280x512 (two tiles) and 1x 1280x1024 (anti-aliasing off). Sessions: the
title and the map zoom-out, the city by day and at night, the pause menu and
the GPS map, a race with its pause menu and replay, 50 switches of each
option, at native and 3x, on both APIs, and with the PS5 host's settings. The
garage was not reached. Resolution scale doesn't enter the key (the sizes are
the game's own, at 720p).

The hook doesn't make one tile when its key isn't cached yet and 7 or more
slots are taken, and logs it once; it never triggered.

## The PS5

The PS5 build sets `edram_tiles = 4096` as a default before it creates the
graphics system (`ps5/game/main_ps5.cpp`, after the host's other settings),
so single-tile rendering is on there too. Checked by reading, as it can't be
built or run here:
- the graphics system is linked into the PS5 binary from the same SDK headers
  (`rex_gpu_create(kGpuPluginAbiVersion, ...)`), so the ABI version matches,
  and `edram_tiles` is registered by its static initializer like the other
  GPU options the host sets;
- the render target cache reads it when the GPU starts, after this point;
- RADV reports `maxStorageBufferRange` 4,294,967,295
  (`ps5/results/mcla-vkinfo-ps5pro-13.42-2026-10-03.txt`), so the 20 MiB buffer
  at native binds whole and there is no fallback;
- `render_perf.cpp`, `perf_options.cpp` and `settings_menu.cpp` are in the
  PS5 build (`ps5/game/build.sh`), so the hook, the anti-aliasing hold and the
  menu row work as on PC;
- the added lines compile against the SDK's `rex/cvar.h` (clang
  `-fsyntax-only`, with 4096 and with `-DMCLA_EDRAM_TILES=2048`).

The PS5 host doesn't use the PC defaults: notably it runs with
`execute_unclipped_draw_vs_on_cpu=false`, which changes how far the render
target cache thinks a draw reaches. So on PC, at native, with the PS5 host's
settings (`execute_unclipped_draw_vs_on_cpu=false`,
`physical_watch_granularity=0`, `shared_memory_request_granularity_log2=16`,
hot pages 4 and 10,000 ms, `shared_memory_invalidation_pages_log2=4`,
`clear_memory_page_state=false`, `vulkan_submit_on_primary_buffer_end=false`,
`edram_tiles=4096`):
- **Vulkan,** one tile against two at frozen time: the city by day and at
  night, the pause menu, the GPS map and the title within frame-to-frame noise;
  the race, its pause menu and its replay look the same;
- **D3D12 frame logs,** the city and the race with one tile, two and
  anti-aliasing off: one tile acts (one bin, 1,378 scene draws instead of
  2 x 1,382 in the city), and the EDRAM transfers are the same in all three
  modes, none in the scene. Scene GPU time at native: city 2.52 → 1.56 ms,
  race 4.68 → 2.86 ms.

Untested on the console: the log line "emulating an EDRAM of 4096 tiles", the
picture and the frame rate need a run there. To go back: `edram_tiles = 2048`
in `/data/mcla/mcla.toml` (the host sets 4096 as a default, which the file
overrides), or build with `-DMCLA_EDRAM_TILES=2048`. It isn't part of the
host's live tuning loop, as it is read only when the GPU starts.

## Reproducing

- **The game side:** nothing extra; `--mcla_single_tile` and the pause menu's
  row switch it during play, `--edram_tiles=2048` turns the larger EDRAM off.
- **Which ranges cross tile 2,048:** `edram-wrap-log.sdk.patch` (SDK, `TMPWRAP`)
  logs every new kind of ownership range and resolve area that does.
- **Resolves:** `direct-resolve-validate.sdk.patch`, with
  `--direct_resolve_validate=true`.
- **GPU time by phase and tile:** as in `single-tile-2026-10-16.md`. The
  analysis scripts were in the session's scratch folder; a tile's scene time
  runs from one phase 0x800 marker to the next.
- **The pads:** generated, not kept; as in `single-tile-2026-10-16.md`, with
  `set mcla_single_tile true|false` and `set mcla_msaa true|false` for the
  modes, then `press start 4f`, `wait 1500ms 60f` twice before the segment's
  `mark`.
- **Runs:** `devrun.py run --fps 0` (or `--fps 120`) at `--res 1` and
  `--res 3` on both APIs, `--fixed-dt 60` for the race. Every run was clean in
  the GPU guard, and the real save data was unchanged.

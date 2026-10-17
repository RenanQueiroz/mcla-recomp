# Why `mcla_single_tile` does nothing, and what one tile is worth (2026-10-16)

**Since then:** single-tile rendering works, in a 4,096-tile EDRAM that the
game asks the SDK for on PC (`patches/rexglue-edram-size.patch`, the hook aimed
at the main scene): `single-tile-edram-2026-10-16.md`. What follows is the
investigation as it was, on the console-sized EDRAM.

`frame-structure-2026-10-16.md` found the main scene still drawn as two
predicated tiles (1280x512 and 1280x208 at 1x, about 1,370 draws replayed per
tile in the city) whatever `mcla_single_tile` is set to. This note finds out
why, tries the single tile by force, and measures what one tile is worth,
with MSAA (estimated) and without (MSAA off, where the game draws one tile by
itself). Windows, RTX 5080, Ryzen 7 9800X3D, launcher settings, built at
`49083cd`. No behaviour changed: the hook's comments were corrected
(`config/render_perf.toml`, `src/render_perf.cpp`).

## Summary

- **The hook is compiled in and called; it always returns at once.** The
  generated code calls `mcla_single_tile` at 0x8217A700 in
  `grcDevice::BeginTiledRendering` (`sub_8217A470`), and `mcla_edram_limit` at
  0x82410E34. The hook acts only when the render target argument (r17) is 0.
  In every session measured, no call passed 0. The main scene's call
  (`sub_822E5ED8`, returning to 0x822E6020) passes its render target too.
  Upstream took the condition from LARecomp, whose own option is off by
  default and "requires the enlarged virtual EDRAM" of its unpublished SDK.
- **It could not work on this SDK anyway.** A 720p 2x MSAA tile needs 1,472
  EDRAM tiles for the colour and 1,472 for the depth. The SDK's EDRAM is the
  console's 2,048 tiles, with addresses wrapping round:
  - `xenos::kEdramTileCount` = 2048, and `kEdramBaseTilesBits` = 11;
  - the resolve shaders wrap their addresses at 10 MiB.

  The comment saying the "runtime's virtual EDRAM is 4096 tiles" was wrong.
  Forced onto the main scene, the colour at tile 1,472 wraps into the depth's
  tiles. The image is right down to row 288 (864 at 3x) and garbage below it,
  and the scene costs 1 ms more GPU time in EDRAM transfers.
- **The guest side would be fine.** The resolve path takes its tile count
  (0x827D42A4) and rectangles from `BeginTiledRendering`. With a count of 1,
  `EndTiledRendering` (`sub_8217B430`) does one bin (0x3) and one full-screen
  resolve. The limit is the host's EDRAM alone.
- **Not fixed: it needs the SDK's EDRAM doubled.** That is an SDK project:
  - every resolve shader rebuilt, on both APIs;
  - the EDRAM buffer and host render targets doubled;
  - the ownership tracking changed;
  - a guest-visible change from the hardware's wrap-around.

  It is set out below. Estimated from the two tiles' GPU times, one 2x MSAA
  tile would save about 0.8 ms of GPU time at 3x in the city by day (10% of
  the GPU frame) and 1.3 ms in the race (12%). It would also take about a
  quarter off the GPU command thread's CPU time.
- **MSAA off is one tile today.** The game computes a 1280x1024 tile, so one
  tile, and the scene is submitted once:
  - draws per frame: 30% fewer in the city, 39% fewer in the race;
  - GPU command thread: 22 to 41% less CPU time;
  - 3x frame time: 8 to 14% shorter in the city, where D3D12 then hits the
    game thread's own limit, and 17 to 18% shorter in the race, on both APIs;
  - native: the frame is 8% shorter on D3D12 and 22% on Vulkan in the race,
    where the GPU command thread is the bottleneck with MSAA on (91 to 93%
    busy).

  The pause menu and the GPS map keep their own three 4x tiles.

## The hook, the call sites and the counts

A development version of the hooks (`single-tile-diag.patch`, `TMPTILE`)
logged every new kind of call:
- the caller, taken from the return address that `__savegprlr_14` stores
  above the 368-byte frame;
- the render target argument;
- the target size;
- the MSAA mode (r18);
- the tile size the game computed;
- the tile count stored at 0x827D42A4;
- whether the hook acted.

It also logged totals every 3,000 calls and every surface whose end the EDRAM
hook saw past 2,048 tiles. The sessions were title, map zoom-out, city, and
for the race runs the pause menu, the arcade menus and the race, on both APIs,
at native and 3x:

| Call (return address) | Render target | MSAA | Tile (1x) | Tiles | Hook acted |
|---|---|---|---|---|---|
| 0x822E6020 (`sub_822E5ED8`), the main scene in play | not 0 | 2x | 1280x512 | 2 | never |
| 0x822E6020, the main scene with MSAA off | not 0 | 1x | 1280x1024 | 1 | never |
| 0x822E6020, the world behind the menus | not 0 | 4x | 1280x256 | 3 | never |
| 0x822E543C (`sub_822E5358`), menus, the zoom-out's start | not 0 | 4x | 1280x256 | 3 | never |

- **`mcla_single_tile`:** thousands of calls a session (6,000 to 9,000
  counted), none with a null render target.
- **`mcla_edram_limit`:** 11 calls a session, none for a surface ending past
  2,048 tiles. Even the forced tile's 1280x736 surfaces (below) never reached
  it: they aren't allocated through the branch of `sub_82410D70` it sits in.

How the tile is computed (`sub_8217A470`, the path for flag bit 0 clear):
- the tile height is the rows that fit in 1,024 tiles for one surface:
  16-row tiles at 1x, 8-row tiles at 2x and 4x, 80 or 40 pixels wide;
- for 1280 wide at 2x that is 512 rows;
- count = ceil(720 / 512) = 2.

A count of 1 at 2x gives a 1280x736 surface pair: depth at tile 0 and colour
at 1,472, 2,944 tiles in all. `EndTiledRendering` loops over the same count,
selects bin `3 << 2i` and resolves each tile's rectangle. Those are the 0x3
and 0xC bins in the frame logs.

## The single tile by force

`mcla_dev_tile_force=1` (the diagnostic patch) applies the hook to the main
scene's call at 2x MSAA, switchable during a run with the pad's `set`.

**The picture.** At 1x and 3x on D3D12 it is right down to row 288 (864 at
3x), black and then garbage below it, including the HUD's lower half. It comes
back when the force is switched off. 288 rows is exactly the colour's share
before the EDRAM wraps:
- the depth is at tile 0 with 1,472 tiles, and the colour starts at 1,472;
- the render target cache gives the last render target the tiles up to the
  first one's base, 2,048 - 1,472 = 576 tiles;
- 576 tiles are 36 rows of 16 tiles, 8 pixel rows each at 2x: 288 rows.

The resolves read the rest from the tiles the depth owns. Screenshots:
`d2_force.png` (1x) and `r1_force.png` (3x) in the session's scratch folder,
not in the repository.

**The cost.** GPU time at 3x, city by day, D3D12, medians of 18 timestamped
frames per mode:

| | Scene draws | Scene GPU | draw | xfer | resolve | Frame (uncapped) |
|---|---|---|---|---|---|---|
| Two tiles (as shipped) | 2,744 | 3.37 ms | 2.37 | 0.00 | 0.42 | 7.51 ms |
| One tile, forced | 1,382 | 4.37 ms | 1.71 | 1.56 | 0.77 | 8.63 ms |

- **Draw time** falls by 0.66 ms.
- **EDRAM ownership transfers** take 1.56 ms where the wrapped colour and the
  depth claim the same tiles.
- **Draws per frame** at 1x: 4,950 to 3,606.
- **GPU command thread:** its CPU time falls by 24 to 30% (D3D12 4.30 to 3.06
  ms per frame at 3x, 4.18 to 3.16 at native; Vulkan 5.21 to 3.67 and 4.85 to
  3.69).

## What one 2x MSAA tile would be worth

Both tiles replay every scene draw. Fitting each tile's scene GPU time as a
fixed part plus a part proportional to its rows (512 and 208) gives:
- the fixed part, per-draw work done once per tile, which one tile saves;
- the per-row part, pixel work that one tile still does.

D3D12, medians of 24 timestamped frames, scene = phases 0x800 to 0x04000000:

| | Tile 1 (512 rows) | Tile 2 (208 rows) | Fixed per tile | One tile (est.) | Saved | Of the GPU frame |
|---|---|---|---|---|---|---|
| City by day, 3x | 2.11 ms | 1.33 ms | 0.80 ms | 2.64 ms | 0.80 ms | 8.04 ms (10%) |
| Race, 3x | 3.60 ms | 2.23 ms | 1.29 ms | 4.54 ms | 1.29 ms | 10.47 ms (12%) |
| City by day, native | 1.30 ms | 0.90 ms | 0.63 ms | 1.57 ms | 0.63 ms | GPU half idle here |

The forced run agrees on the draw part: the model gives 1.82 ms of draw time
for one tile by day, and the forced tile measured 1.71. MSAA off saves 1.02
ms (day) and 1.76 ms (race) of the same scene, so one 2x tile would keep about
three quarters of MSAA off's GPU saving while keeping the anti-aliasing. It
would also take most of its CPU saving, as the forced runs show.

## What a working single tile needs

All of it is in the SDK, on both APIs:

1. **A host EDRAM of 4,096 tiles.**
   - `xenos::kEdramTileCount` sizes the ownership map, the wrap-around in the
     render target cache, the transfer and dump shaders made at run time, and
     the ROV/FSI pixel shaders' EDRAM addressing (`dxbc_translator_om.cpp`,
     `spirv_translator_rb.cpp`).
   - The EDRAM buffer would be 20 MiB at 1x and 180 MiB at 3x.
   - About 90 uses of the EDRAM constants.
2. **A 12-bit base.** `kEdramBaseTilesBits` = 11 now:
   - `RenderTargetKey` has room (26 of 32 bits);
   - the transfer constants' `source_to_dest` and `dispatch_first_tile`
     fields, and Vulkan's packed keys, need a bit each.
3. **Every resolve shader rebuilt.** The 22 EDRAM-buffer resolve shaders per
   API (fast, full and clear, scaled and unscaled) wrap at 10 MiB (`udiv ...
   0x00280000`) and decode an 11-bit base at bit 13 of
   `xe_resolve_edram_info`. The 24 direct resolve shaders per API decode the
   same field. They would be rebuilt from the xenia 0b2ffa3 sources as the
   fork's other shaders were, then validated byte for byte with
   `direct-resolve-validate.sdk.patch`.
4. **Twice the VRAM for host render targets.** They are as tall as the whole
   EDRAM from their base (`GetRenderTargetHeight`), so each doubles. A
   1280-wide 2x target at 3x would go from 3072 to 6144 rows. Either that, or
   size them by use, a larger change.
5. **An opt-in setting.** The console wraps at 2,048 tiles, and a title that
   relies on that would break, so the larger EDRAM has to be off by default
   for other titles.
6. **The game hook.** It should act on the main scene's call at 2x MSAA, not
   on r17 = 0. The 4x menu passes would need 5,760 tiles and would stay
   tiled. Identifying the call needs r1 or r18 among the hook's registers,
   which is a codegen change.
7. **Checks.** Resolves and transfers, screenshots on both APIs in every
   scene, and both debug layers.

This is several days to two weeks of work, touching every resolve and
transfer path, and it diverges from the hardware, so upstreaming it is
uncertain. It is not something to do safely inside this investigation. The
in-game PERFORMANCE › SINGLE TILE RENDERING row and the `mcla_single_tile`
setting do nothing meanwhile. They are left as they are; removing them or
turning them off is upstream's call.

## MSAA on and off

Measured by switching `mcla_msaa` with the pad's `set` within one run, so both
modes see the same scene:
- **City:** the city by day (13:00, set by poking the time of day), standing
  at the spawn point. Four alternations of about 6 s per mode, uncapped.
- **Race:** the race benchmark (Sunset and Vine, midnight, rain, 7
  opponents, timedemo at 60 updates a second, uncapped). Four alternations of
  600 updates per mode.

How each column was measured:
- **Frame:** the median swap interval, leaving out each segment's first 40
  swaps.
- **Draws:** the SDK's per-frame counter.
- **GPU busy:**
  - D3D12: timestamped frames from `frame-log.sdk.patch`, with intervals over
    0.5 ms counted as idle;
  - Vulkan: whole-frame timestamps from `gpu-timestamps-vulkan.sdk.patch`,
    one a second;
  - both include the timestamps' own cost.
- **GPU command thread:** its CPU time per swap, sampled from Windows
  (`GetThreadTimes` on the thread named "GPU Commands"), with no SDK patch.

| | Frame on → off | Draws on → off | GPU busy on → off | Scene GPU on → off | GPU command thread on → off |
|---|---|---|---|---|---|
| City, D3D12 3x | 7.51 → 6.94 ms (−7.6%) | 4,501 → 3,152 | 8.04 → 7.27 ms | 3.43 → 2.41 ms | 4.36 → 3.38 ms (−22%) |
| City, Vulkan 3x | 8.29 → 7.16 ms (−13.6%) | 4,553 → 3,185 | 8.71 → 7.49 ms | | 5.33 → 3.78 ms (−29%) |
| City, D3D12 native | 6.87 → 6.84 ms | 4,536 → 3,169 | 4.58 → 3.79 ms | 2.24 → 1.42 ms | 4.32 → 3.10 ms (−28%) |
| City, Vulkan native | 6.80 → 6.90 ms | 4,551 → 3,176 | 4.69 → 3.97 ms | | 4.96 → 3.53 ms (−29%) |
| Race, D3D12 3x | 9.54 → 7.85 ms (−17.7%) | 6,358 → 3,896 | 10.47 → 8.66 ms | 5.87 → 4.11 ms | 7.57 → 5.00 ms (−34%) |
| Race, Vulkan 3x | 11.41 → 9.49 ms (−16.8%) | 6,467 → 3,973 | 11.83 → 9.53 ms | | 9.25 → 5.50 ms (−41%) |
| Race, D3D12 native | 7.46 → 6.83 ms (−8.4%) | 6,280 → 3,902 | | | 7.00 → 4.55 ms (−35%), 91 → 66% busy |
| Race, Vulkan native | 8.87 → 6.89 ms (−22.3%) | 6,070 → 3,908 | 7.28 → 5.76 ms | | 8.31 → 5.55 ms (−33%), 93 → 80% busy |

How to read it:
- **Tiles:** 2 with MSAA on, 1 with it off, in every one of these runs.
- **The frame's limits:**
  - In the city at native, both APIs, and at 3x on D3D12 with MSAA off, the
    frame is the game's own limit, about 6.85-6.95 ms (144-146 FPS). The
    savings there show only in the GPU and GPU command thread columns.
  - In the race at native, the GPU command thread is the bottleneck with MSAA
    on, and MSAA off lifts the frame to that same limit.
- **Older figure:** README.md's "about 20% faster at 3×" for anti-aliasing
  off dates from before the clear-rectangle and direct-resolve work, and is
  now 8 to 18%.
- **A caveat for any repeat with forced segments:** MSAA-off segments that
  followed a forced single-tile segment carried 0.65 ms of EDRAM transfers in
  the scene (at phases 0x10000, 0x20000, 0x800000 and 0x02000000), which runs
  without forcing don't have. The figures above come from runs without
  forcing.

**For the upscaler and anti-aliasing plan.** MSAA off plus a temporal
anti-aliasing pass at full resolution is the performance path available now:
- one tile;
- 30% less scene GPU time at 3x by day and in the race on D3D12;
- 22 to 41% less GPU command thread time.

A working 2x single tile would keep MSAA and about three quarters of that GPU
saving, but only after the SDK project above.

## Reproducing

- **The game side.** `single-tile-diag.patch` (game, `src/render_perf.cpp`):
  the `TMPTILE` log lines and `mcla_dev_tile_force` (1 = the main scene at 2x,
  2 = every call), then `scripts\build.ps1 -NoCodegen`. It was written for the
  old hook and no longer applies; it is in the history at commit `1600751`.
- **GPU timestamps by phase and tile.** On D3D12, apply `gpu-timestamps.sdk.patch`
  and `frame-log.sdk.patch`, as in `frame-structure-2026-10-16.md`. On Vulkan,
  apply `gpu-timestamps-vulkan.sdk.patch`. Run with:
  - `--tmp_frame_log_dir=<dir>`;
  - `--mcla_render_markers_all_phases=true` (every phase's marker; the
    markers are in the build since 2026-10-16, `src/render_markers.cpp`;
    this note's runs used the development `render-markers.patch` and
    `--mcla_dev_render_markers=true`, which they replaced);
  - on Vulkan, `--env MCLA_GT_EVERY=1 --env MCLA_GT_DIR=<dir>`.

  A tile's scene time runs from one phase 0x800 marker to the next. The
  markers are replayed with the tile.
- **The pad scripts.** Generated, not kept:
  - the city: boot to roam, then `poke [0x8287E26C]+0x38CC 0x41500000` and
    the same at `+0x38DC` (13:00);
  - the race: `boot-to-race.pad`, `follow sunset-and-vine.route`, and the
    start condition of `race.pad`;
  - then per mode: `mark <mode>_<n>`, `set mcla_msaa true|false` (and
    `set mcla_dev_tile_force 0|1`), a wait, six
    `set tmp_frame_log <label>` 300 ms or 40 updates apart, a wait.
- **Runs.** `devrun.py run --fps 0` at `--res 1` and `--res 3` on both APIs,
  and with `--fixed-dt 60` for the race. Every run was clean in the GPU guard,
  and the real save data was unchanged.
- **Reverting.** Reverse the patches and run `build-game.ps1 -ResetSdk`.

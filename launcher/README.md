# Midnight Club: Los Angeles launcher

Part of this fork (see `..\FORK.md`); Windows only. Desktop shortcut →
`launcher\app\MclaLauncher.exe`. It opens on the game's settings; **Play**
starts `mcla.exe`. On start it fetches the fork
([RenanQueiroz/mcla-recomp](https://github.com/RenanQueiroz/mcla-recomp)); when
the fork has new commits it offers **Update and rebuild**, with the commits folded
under *What's new*. Routine states (checking, up to date, offline) are a line
under the title. *More* links to the fork's recent changes and says how many
commits upstream ([holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp))
has that the fork has not taken yet: the fork takes them only when asked to.
A commit that changes only the launcher or documentation doesn't ask for a game
rebuild.

```
..\..\                               the folder the repository is in
├─ *.iso                             your disc image (only read until game\ is extracted)
└─ mcla-recomp\                      the fork's checkout; game\ and out\ live in here
   └─ launcher\
      ├─ build-game.ps1              update + build; the launcher runs this
      ├─ build-launcher.ps1          rebuild the launcher itself, recreate the shortcut
      ├─ update-launcher.ps1         rebuild and restart it after an update changed it
      ├─ src\                        launcher source (C#, Reactor on WinUI 3)
      ├─ dev\                        measurement tools (below)
      ├─ app\                        the built launcher                      (not tracked)
      ├─ logs\                       one log per build the launcher ran      (not tracked)
      ├─ settings.json               launcher options                        (not tracked)
      └─ built-commit.txt            commit of the last successful build     (not tracked)
```

## From a terminal

```powershell
.\build-game.ps1             # build (or finish building) what is checked out
.\build-game.ps1 -Update     # fast-forward to the fork's latest commit, then build
.\build-game.ps1 -ResetSdk   # re-apply the SDK patches and rebuild the SDK
```

`build-game.ps1` wraps `scripts\make_pc.ps1` and adds what it leaves out:

- **Changed SDK patches.** `make_pc.ps1` patches the ReXGlue SDK once and adds
  later patches by a name each one adds, so a patch that changed would never be
  applied again. When an update changes `patches\*.patch` the SDK checkout is
  reset and patched again (incremental SDK rebuild). If the SDK version itself
  changes, the SDK is re-cloned.
- **Local edits.** Updates use `git merge --ff-only --autostash`. Codegen rewrites
  `mcla_manifest.toml` and `generated\rexglue.cmake` with LF line endings, so they
  always look modified; autostash carries them across updates safely.
- **Python.** `make_pc.ps1` rejects any `python.exe` under `WindowsApps`, which is
  where the Python install manager puts its working one; the wrapper hands that one over.

The fork's changes to the game and the SDK are ordinary commits, each described
in `KNOWN-ISSUES.md` (game) or `patches\README.md` (SDK).

## The game's settings

The drop-downs are the game's own settings, in `mcla.toml` next to `mcla.exe`
(`out\build\win-amd64-release\`): the same file and values as the Settings tab
of the game's pause menu (DISPLAY, PERFORMANCE, CONTROLS), so each shows what the
other set. A drop-down shows the file's value, or the game's default while the
file doesn't set it; a value set by hand that isn't one of its choices shows as
"... (set in mcla.toml)". A change is written at once, the way the game writes
(`rex::cvar::SaveConfigValues`, from `patches\rexglue-cvar-toml-strings.patch`):
the key's `key = value` is replaced where it is, keeping a comment after it, or
added before the first `[table]`; every other line is kept (comments, keys it
doesn't know, tables, values over several lines, a BOM, CRLF), and the file is
written beside the old one, flushed to the disk and renamed over it
(`src\GameSettings.cs`). A file that isn't UTF-8 text is left as it is, with
a message, rather than rewritten; a value it can't read, such as a broken
escape, shows as set by hand.

While the game runs (started here, from its shortcut, or again by its own
RESTART GAME; any `mcla.exe` counts, checked every two seconds) the drop-downs
are greyed out with a note to use the pause menu. The game reads the file only
when it starts, and its pause menu writes it too, so edits here would neither
reach the running game nor be safe from being overwritten. When it closes, the
file is read again and shows what was changed in the game.

The launcher passes only what is typed under *Advanced*, **More mcla.exe
options**; an option there wins over `mcla.toml` and the pause menu at every
start, also after the pause menu's RESTART GAME, which starts the game again
with the same options: `--resolution_scale` there overrides the pause menu's
RESOLUTION. It no longer passes `--vsync=false`: the game turns vsync off by itself
unless the pause menu's VSYNC (or `vsync = true` in the file) turns it on, and
with it on the GPU plugin's emulated 60 Hz vblank caps the game at 60 and paces
it unevenly. (An earlier note here said vsync set from the game never reached
the GPU plugin; rechecked on 2026-10-16, it does, at start and from the pause
menu.)

Launchers before 2026-10-16 passed every choice on the command line at every
start, which beat what the pause menu saved. The first time this one opens it
moves those choices into `mcla.toml`, once (`SettingsMigration` in
`src\GameSettings.cs`):

1. it keeps a copy of `mcla.toml` as `mcla.toml.before-launcher-settings`, next
   to it, before writing anything (an empty file when there was no
   `mcla.toml`), and takes the copy back if the write fails, to try again at
   the next start;
2. it writes each choice that was forcing a value: every one but "Game setting",
   and Resolution and HUD updates, which had no "Game setting"; and
   `vsync = false` with any frame rate, which came with `--vsync=false`;
3. it deletes nothing, and drops the old choices from `settings.json`, which
   then holds `SettingsInGameFile: true`.

It runs only once: not when the copy already exists or the flag is set, and
not from a `settings.json` it can't read (whose choices are unknown; it is
kept as `settings.json.unreadable` before the defaults replace it). A choice
made in the launcher before then also ends it. It doesn't run while the game
is running, which writes the file itself: it waits until the game has closed.
A new install (no `settings.json`) has nothing to move. The old HUD updates
choice (`HudUpdates`, an on/off switch) isn't carried over; the new one is a
rate, 30 per second unless set.

If `settings.json` can't be saved, the launcher says so instead of carrying on
as if it had.

*Display gamma* (`mcla_display_gamma`) picks the gamma ramp the picture goes
through on its way to the screen, and it switches during play too (the pause
menu's DISPLAY > DISPLAY GAMMA). *TV* (`tv`, the default) is the console's ramp
for an HDTV (BT.709), as before; *PC monitor* (`pc`) is sRGB, which leaves the
game's own sRGB-encoded output as it is, as a PC monitor expects. On a PC
monitor the TV ramp darkens the shadows: 8-bit levels 64, 128 and 192 come out
as 48, 115 and 185. In a night scene the average brightness went from 46 to 60
of 255 with *PC monitor* (`KNOWN-ISSUES.md`).

*Anti-aliasing* is the game's 2× MSAA (`mcla_msaa`), and it switches during play
too (the pause menu's PERFORMANCE > ANTI ALIASING). Off, a 3× frame is 2 to 5%
shorter than on with single-tile rendering (2.1 and 3.1% in the city by day on
D3D12 and Vulkan, 2.7 and 5.3% in the race), for slightly more jagged thin edges
such as pylons and billboard frames; the dithered foliage looks cleaner. Against
anti-aliasing on in two tiles it was 8 to 18% (`dev\single-tile-2026-10-16.md`).

*Single tile rendering* (`mcla_single_tile`, on by default) draws the scene with
anti-aliasing on once instead of in the Xbox 360's two tiles, in the larger
EDRAM (4,096 tiles instead of the console's 2,048) the game asks the GPU plugin
for on PC (`edram_tiles`). It switches during play (the pause menu's PERFORMANCE
> SINGLE TILE RENDERING). Against two tiles, a 3× frame is 9 to 11% shorter in
the city by day and 15 to 20% in the race, the GPU command thread uses 28 to
35% less CPU time, and at native the race is 14% (D3D12) to 25% (Vulkan) faster;
with a 120 FPS cap at 3× in the city, 99% of frames are within 1 ms of the target
instead of 93 to 94% (`dev\single-tile-edram-2026-10-16.md`). Off, the scene is
drawn as on the console. With `--edram_tiles=2048` there is no room for one tile:
the pause menu's row says UNAVAILABLE and nothing changes either way.

*Graphics API* picks the GPU plugin's backend (`mcla_gpu_backend`: `any`, which
tries Direct3D 12 first, `d3d12` or `vulkan`). Every setting and SDK patch here
works on both. On this machine, in scripted runs, Vulkan matches D3D12 at native
resolution; at 3x the two are within a few percent in the city (Vulkan slightly
ahead in free roam), and D3D12 is 10 to 15% faster in the race benchmark
(`dev\direct-resolve-2026-10-15.md`). Since D3D12's textures are placed in
shared heaps, both are about as smooth in the map zoom-out after the title
(`dev\d3d12-zoomout-2026-10-15.md`).

*HUD updates* (`mcla_ui_hud_rate`) sets how many times a second the HUD's
Flash movies (gauges, minimap, song ticker, the LOADING badge) take a step: 30
(the default, as on the console), 60, 120, or every frame (0). The ticker scrolls
one step at a time and the needles move only when it steps, so at 30 the ticker
scrolls at its original speed and the needles move in 30 Hz steps; 60 and 120
make the needles smoother and the ticker two and four times as fast (at a 120 FPS
cap: 59, 118, 237 and 239 px/s at 720p for 30, 60, 120 and every frame). Menus,
pop-ups and the GPS panels keep the console's pace whatever it is set to
(`mcla_ui_timing_fix`, `KNOWN-ISSUES.md`).

The launcher also used to pass four SDK settings that measured faster (in-run
A/B; the first two are ideas from
[ForzaHorizon1-ReImagined](https://github.com/Dyotson/ForzaHorizon1-ReImagined)).
They are now the game's own defaults on PC (`MclaApp::ApplyPcDefaults` in
`src\mcla_app.cpp`), used unless the command line, an environment variable or
`mcla.toml` sets them:

- `clear_memory_page_state=false`: guest memory already uploaded to the GPU
  stays valid from one frame to the next instead of being copied again every
  frame (2.5 → 0.8 GB/s of uploads; 10 to 21% less GPU command thread time at
  Native, uncapped, depending on the scene). mcla-recomp's PS5 port runs this way too.
- `execute_unclipped_draw_vs_on_cpu=true` (Xenia's default): draws with
  clipping off no longer claim the whole EDRAM, which saves render-target round
  trips (about 11% faster at 3×; no change at Native).
- `shared_memory_hot_page_faults=4`, `shared_memory_hot_page_ms=10000` (hot
  pages, as mcla-recomp's PS5 port runs): a page the game keeps rewriting is
  uploaded whenever a draw uses it instead of being write-protected again after
  every write fault. With the profiling notes' fast-path patch, write faults go
  from about 12,000 to about 200 a second and protection changes from about
  190,000 to 1,000-2,000; frames on the title screen at Native, uncapped, were
  about 9% shorter (6.87 against 7.57 ms, in-run switch every 20 s, both halves
  agree). Needs `rexglue-hot-pages-watched-ranges.patch`: without it, textures
  the CPU rewrites (the attract movie's colour planes) stopped updating.

To change one, type it under *More mcla.exe options* (e.g.
`--clear_memory_page_state=true` if geometry or textures ever look stale, or
`--shared_memory_hot_page_faults=0` to turn hot pages off). mcla.exe's
command-line parser doesn't take an option twice (CLI11 rejects a repeated
option by default; a boolean given twice adds up).

Fixed interaction (2026-10-14): uncapped with the intro at "Normal", about one
frame in four took 33 ms. mcla-recomp holds loops that never reach the frame
timer (start-up logos, loading screens) to 30 FPS, and took any single swap
without a timer pass for one; uncapped, the game's update and presenting threads
drift against each other and produce such swaps in normal play. The hold now
needs two in a row (`KNOWN-ISSUES.md`, "Loading-screen pacing"). The
"Fast" and "Skip" intros now speed up only the start-up logos, not loading
screens later.

## Rebuilding the launcher

Needs the .NET 10 SDK (`winget install Microsoft.DotNet.SDK.10`). Close the
launcher, then run `.\build-launcher.ps1`. After an update that changed the
launcher, it offers **Restart to update the launcher**, which does the same via
`update-launcher.ps1` once it has closed. `build-launcher.ps1` records the last
commit that changed the launcher in `app\source-commit.txt` for that check.

Package versions float: each build takes the newest Reactor preview and the newest
stable Windows App SDK 2.x. Reactor is pre-1.0 and a new preview can change its API,
so if a rebuild suddenly fails to compile, check what changed in
`%USERPROFILE%\.nuget\packages\microsoft.ui.reactor\<version>\agentkit\reactor.api.txt`
(the index for the version actually used; Reactor's GitHub main runs ahead of it).

The icon is `src\launcher.ico`.

## Measurement tools

`dev\` holds what the measurements in this fork were made with; nothing reads it
during a build:

- `ab-harness.patch` (`src\perf_options.cpp`): flips cvars in-run every
  `MCLA_AB_SECONDS` (default 4) seconds, e.g.
  `--mcla_ab_test=set:gpu_regwrite_bulk=on/off`, so an A/B compares the same scenes.
  Run uncapped with `--mcla_intro=fast` and `REX_PERF_LOG_CSV`, and leave out
  the first 20 s and the attract video (about 120-131 s after launch). It also
  sets `tmp_ab_state` to the current side for SDK-side logs that have that cvar.
- `vulkan-submission-counters.sdk.patch` (SDK, Vulkan) with `cyc-ab.py`: every
  5 s a `TMPSUB` log line with the GPU command thread's CPU time, draws,
  command-buffer recording, `vkQueueSubmit` and fence-wait time per frame;
  with `TMPSUB_CSV=<file>`, one row per swap with the thread's cycle count and
  the A/B side, which `cyc-ab.py` compares per draw between neighbouring
  windows. Thread cycles measure the GPU command thread's own cost even when
  the frame rate is held by another thread.
  Stale: it no longer applies to the patched SDK, whose Vulkan submission code
  it changes has moved since.
- `draw-cost.py`: fits frame time = fixed + per draw x draws over a run's
  `REX_PERF_LOG_CSV` output.
- `dropped-patches\`: patches that were measured and not kept: specialized
  system constant updates, sampler fetch-constant prefetch, reused texture
  request queues, skipping the clock in shared-memory requests, keeping bindings
  for fetch constants rewritten unchanged (ForzaHorizon1-ReImagined's patch 0032,
  -0.4%), the achievement-toast removal the on-demand toast replaced,
  byte-exact uploads of hot pages for D3D12 (no change in CPU time), and the
  block-by-block register writes of `rexglue-gpu-regwrite-bulk.patch` ported to
  Vulkan (`rexglue-vulkan-regwrite-walker.patch`: -0.5% +/- 0.6% GPU command
  thread time per draw on top of the patch's common part, which gives Vulkan
  its -9.1%). The PS5
  hot-page write-watch settings (`--shared_memory_hot_page_faults=4`) measured
  slightly worse on PC before `rexglue-shared-memory-fast-path-hot-pages.patch`;
  with it they take 20% off the GPU command thread (see the profiling notes).
- `profiling-2026-10-14.md`: where the time goes on the CPU at native resolution
  and on the GPU at 3x, with the instrumentation that measured it
  (`upload-counters.sdk.patch`, `gpu-timestamps.sdk.patch`, `gt-analyze.py`).
- `vulkan-2026-10-14.md`: the same settings and patches measured on Vulkan,
  where its GPU command thread's time goes, validation, pacing and stress runs,
  the render pass breaks that upload hoisting removed, the Vulkan submission
  thread, the texture copy that held Vulkan back at 3x, and a D3D12 against
  Vulkan benchmark in the city.
- `race-benchmark-2026-10-15.md`: the race benchmark (`scripts\race.pad`):
  how it gets into the race, how repeatable it is, D3D12 baselines and where
  the GPU time goes in it at 3x.
- `gpu-timestamps-vulkan.sdk.patch` (SDK, Vulkan): the frame timings of
  `gpu-timestamps.sdk.patch` for Vulkan (`MCLA_GT_EVERY`, `MCLA_GT_DIR`), also
  bracketing render pass begins and ends; applies together with the D3D12 one.
- `d3d12-zoomout-2026-10-15.md`: why D3D12 stuttered in the map zoom-out (and
  in races) and Vulkan did not, the placed texture heaps that fixed it, and
  their benchmark.
- `d3d12-hitch-timers.sdk.patch` (SDK, D3D12, `TMPHITCH`): CPU timers around the
  D3D12 calls that can stall the GPU command thread (texture creation and
  release, tile heaps, render targets, fence waits and more), one log line per
  slow frame; applies on top of `rexglue-d3d12-placed-texture-heaps.patch`.
- `texload-split-timestamps.sdk.patch` (SDK, D3D12 and Vulkan): on top of the
  two `gpu-timestamps` patches, times the steps of every texture load
  separately (the barriers, the decode dispatches, the copy into the image) and
  labels each load with its size and format; `texload-split.py <dir>` sums
  them per frame and per kind of load, with the rate of each step in GB/s. It
  found Vulkan's `vkCmdCopyBufferToImage` copying at about 120 GB/s
  (`rexglue-vulkan-texture-load-storage-copy.patch`).
- `edram-transfers-2026-10-15.md`: where the EDRAM ownership transfers came
  from (the game's clear rectangles at 4x MSAA), the change that removed most
  of them (`rexglue-clear-rectangle-draws.patch`) and its benchmark.
- `edram-transfer-log.sdk.patch` (SDK, D3D12, `TMPXF`): logs every draw that
  changes the bound render targets or causes transfers, with the state that
  decides whether it overwrites everything it covers, every
  `MCLA_XF_EVERY`-th frame to `MCLA_XF_FILE`; `xf-classify.py <log>` sorts
  the transferred samples by what triggered them. Applies on top of
  `rexglue-clear-rectangle-draws.patch`.
- `resolves-2026-10-15.md`: what the resolves and the texture reloads at 3x
  are, the direct texture loads that halved the reloads' cost
  (`rexglue-texture-load-direct.patch`) and their benchmark, and the options
  left.
- `resolve-split-timestamps.sdk.patch` (SDK, D3D12 and Vulkan, `TMPR4`): on top
  of `rexglue-direct-resolve.patch` and the two `gpu-timestamps` patches (and
  `texload-split-timestamps.sdk.patch` for the loads), labels each resolve
  (destination, size, formats, EDRAM range, MSAA, sample) and times its steps
  (the dump into the EDRAM buffer, the resolve shader, the direct resolve
  shader `rdirect`, the clear); `resolve-split.py <dir>` sums them per frame
  and per kind, with the texture loads by address.
  `texload-split-timestamps.sdk.patch` now also marks loads done by
  `texture_load_direct` (`direct=1`).
- `shader-stutter-2026-10-15.md`: the stalls while new pipelines compile
  (first sessions, new places, after driver updates), the patches that
  removed them (`rexglue-async-pipelines-no-frame-wait.patch`,
  `rexglue-vulkan-pipeline-storage-parallel-translation.patch`,
  `rexglue-vulkan-pipeline-cache-file.patch`) and the cold, post-update and
  warm sessions they were measured in.
- `shader-stutter-timers.sdk.patch` (SDK, D3D12 and Vulkan, `TMPSS`): a
  `tmp_shader_salt` setting that makes the translated shaders differ for the
  driver's cache (a cold driver cache without emptying it), and log lines for
  every pipeline created and every frame that left draws out, waited or did
  pipeline work on the GPU command thread. Applies on top of the three patches.
  `devrun.py --cache-root <folder>` runs with a shader storage of its own.
- `direct-resolve-2026-10-15.md`: resolves that read the render target
  directly instead of dumping it into the EDRAM buffer
  (`rexglue-direct-resolve.patch`) at any resolution scale, and the 4x MSAA
  depth samples the EDRAM path misread
  (`rexglue-resolve-4xmsaa-depth-samples.patch`): the byte-for-byte
  comparisons, GPU time per resolve, the benchmarks and the checks.
- `direct-resolve-validate.sdk.patch` (SDK, D3D12 and Vulkan, `TMPDV`): on top
  of `rexglue-direct-resolve.patch`, a `direct_resolve_validate` setting: one
  direct resolve a frame, each in turn, is also done through the EDRAM buffer
  into the same destination (the scaled resolve buffer, or shared memory at
  native resolution), and both results are read back and compared byte for
  byte; differences, and every 50 a summary per kind (copy shader, MSAA,
  formats, samples, conversion), go to the log. With
  `rexglue-resolve-4xmsaa-depth-samples.patch` in, no difference is expected.
- `single-tile-2026-10-16.md`: why `mcla_single_tile` never took effect (no
  call passes a null render target, and the SDK's EDRAM was 2,048 tiles, too
  few for a 720p 2x MSAA tile), what one tile would be worth and what it
  needs, and MSAA on against off (one tile) on both APIs. Fixed since (next
  entry); its diagnostic game patch, `single-tile-diag.patch`, was for the old
  hook and is only in the history.
- `single-tile-edram-2026-10-16.md`: single-tile rendering that works, in a
  4,096-tile EDRAM (`rexglue-edram-size.patch`): what changed in the SDK and
  the game, the checks, one tile against two (and anti-aliasing off) on both
  APIs at native and 3x, and with a 120 FPS cap; why mode switches without the
  pause menu can leave the vehicles in the other MSAA mode.
- `edram-wrap-log.sdk.patch` (SDK, `TMPWRAP`): logs every new kind of EDRAM
  ownership range or resolve area that crosses tile 2,048 (with the console's
  EDRAM, the ranges that wrap around), with a count every 5,000. Goes on top of
  `rexglue-edram-size.patch`.

## Scripted controller input (unattended runs)

A development tool of this fork only; it is never part of an upstream pull
request. `src\dev_pad.cpp` adds a synthetic controller to the game when one of
its options is set, and does nothing otherwise. It drives the game for
unattended runs and benchmarks: through the menus, through the map zoom-out
loading screen, into the city and around it, with or without the game window
in front. A real controller keeps working alongside it.

- `--mcla_pad_script=<file>` runs a script from start-up,
  `--mcla_pad_pipe=mcla-pad` takes commands on `\.\pipe\mcla-pad`
  (`dev\pad.py press start`, `pad.py shot C:\temp\a.png 0.5`, `pad.py -i`), and
  `--mcla_pad_frames_csv=<file>` logs every swap with the script's `mark`s. The
  commands are listed at the top of `src\dev_pad.cpp`: buttons, triggers and
  sticks, waits in time, engine frames or both (`wait 300ms 18f`: until both
  have passed, for menus at low frame rates), `wait_until` / `press_until` a
  condition (`left_title`, `loading`, `roam`, or a guest memory word),
  screenshots, recording and playback. A line that fails (a condition timing
  out, an included script failing) ends the script, and with
  `--mcla_pad_exit_on_end` the game; `devrun.py` then reports the run as
  failed. `set <option> <value>` changes an
  option while the game runs, for comparing two settings within one run (the
  time of day and the traffic differ from run to run). `close` closes the
  window as its close button does (waiting for a save being written; `quit`
  doesn't), and `saving` says whether one is (`saving wait`: until one is).
  `--mcla_fixed_dt=N` makes every game update advance exactly 1/N s:
  uncapped, a timedemo.
- `dev\devrun.py` runs the game with a script **on a copy of the save data**:
  `prepare` snapshots `Documents\mcla` (career save, achievements) to
  `%LOCALAPPDATA%\mcla-dev\pristine`; every run restores
  `%LOCALAPPDATA%\mcla-dev\work` from it and passes `--user_data_root` and
  `--cache_root` there, backs up `mcla.toml` next to `mcla.exe` and puts it back
  after the run, and stops if the real save, achievements or `mcla.toml`
  changed. `--toml <file>` starts a run with that `mcla.toml` instead (`none`:
  with none), and what the game saved there (the pause menu's settings) is kept
  as `mcla.toml.after` in the run folder. `--vsync unset` leaves `--vsync` off
  the command line, which otherwise has `--vsync=false`.
  A RESTART GAME in the run (a new mcla.exe started by the game) is followed
  until the last one exits, before `mcla.toml` is put back and the checks run.
  `run` runs one script, `bench` repeats one per configuration
  (alternating, e.g. `--configs d3d12:1,vulkan:1`) and reports each segment's
  median frame time, 1% low and run-to-run spread, `analyze` reads a run.
  `devrun.py prepare --refresh` takes a new snapshot after you have played on.
  Every run is also checked for other programs using the GPU (the GPU guard,
  below).
- `dev\scripts\`: `boot-to-roam.pad` (title, the map zoom-out loading, the
  city: the loading benchmark), `drive-spawn.pad` (the same, then about 8 s of
  driving from the spawn point; with `--fixed-dt 60 --track` three runs' paths
  stayed within 1.7 m of each other), `mark-roam.pad` (for recordings) and
  `race.pad` (an arcade race, "Sunset and Vine" with 7 opponents, traffic,
  rain and midnight, through `boot-to-race.pad`; the car follows
  `sunset-and-vine.route` with the `follow` command; see
  `dev\race-benchmark-2026-10-15.md`). Scripts can `include` others.
- `dev\route.py record <file>` writes the path the leading opponent drives in
  a race (from the pipe's `cars` command) as a route for `follow`.

The game ignores buttons during its own transitions (the title screen's camera
cuts, a menu closing), so scripts press until a condition holds rather than
once. Benchmarks run with `--mcla_intro=normal` (`devrun.py bench`'s default):
"fast" also lets the loading loops run unpaced.

### GPU guard (other programs on the GPU)

Another program using the GPU during a run (a second game, a video, a stress
test) takes GPU time from the game and skews its frame times. While the game
runs, `devrun.py` samples every process's GPU use once a second from Windows'
`\GPU Engine(*)\Utilization Percentage` counters through PDH (no admin rights;
about 1 ms a read). It adds up each process's 3D and compute engines (copy and
video engines are left out) and ignores the game, its child processes and
`dwm.exe`, which composes the game's window and gets busier with it. Another
process above `--gpu-threshold` (default 5, in % of one engine) over the whole
run or over any 5 s marks the run as **contaminated**:

- the run's summary line (`run`, `bench`, `record`) says whether it was clean,
  and lists the offending processes with their pid, mean, highest 5 s and peak
  use, and the game's own use;
- `gpu-guard.json` in the run folder holds the verdict and those figures for
  every process seen; `devrun.py analyze <run>` prints it.

Before each run the guard samples for 2 s. If another process is busy, it says
so and waits up to `--gpu-wait` seconds (default 0; 120 for `bench`) for it to
stop. If it doesn't stop, the run goes ahead and is marked (`busy_at_start`).

`bench` redoes a contaminated run straight away, so the configurations still
alternate, up to `--gpu-retries` redos in all (default 3; warm-up runs are not
redone; a redo's folder ends in `-redoN`). Contaminated runs are left out of the
statistics, and the summary says how many runs were redone and why. Past the
cap it says the comparison is NOT RELIABLE, with the runs left out and who was
using the GPU.

`--no-gpu-guard` turns the guard off. Where the counters are not available
(some sessions, or GPU performance counters turned off) it warns once and the
runs go on unchecked. `devrun.py gpu [--seconds 10]` lists who is using the GPU
right now. `dev\devrun_test.py` tests the verdicts and `bench`'s redos on
synthetic samples.

Measured on this machine (RTX 5080, 2026-10-16):

- **Background levels.** Idle and during runs, no other process went above
  2.3% over 5 s: Chrome, an Edge window animating a full-screen canvas (1.8%),
  the NVIDIA overlay, Windows Terminal, Explorer. dwm.exe peaked at 3%. The
  game itself reads 50-70% at Native (CPU-bound) and about 77% at 3x.
- **A light load.** A compute load of about 9% was flagged in every run, yet
  over a whole run it averaged only 3.5-6%, which is why the 5 s window counts.
  It did real damage: at 3x (`drive-spawn.pad --fixed-dt 60`) the city
  segments' 1% low fell to 54-56 and 82-83 fps in two runs against 87-90 clean,
  with medians within 2%. At Native, which is CPU-bound, the medians moved by
  only 0.4%. 5% is twice the busiest harmless process above.
- **Cost.** Guard on against off, 4 alternating runs each of
  `drive-spawn.pad --fixed-dt 60` at Native: the city segments' medians agreed
  within 0.2% (6.864 against 6.880 ms), inside the run-to-run spread.

### Recording a drive for playback

    python launcher\dev\devrun.py record C:\Users\<you>\Desktop\drive.csv

starts the game (Direct3D 12, Native, 120 FPS cap, windowed; `--fullscreen`,
`--res 3`, `--backend vulkan` as you like) on the save copy, so nothing you do
in that session reaches your real career. Press Start at the title as usual and
drive with your controller; the recording starts counting when you are in the
city. Close the game when you are done. The file holds your controller's state
frame by frame and the car's position twice a second. A run replays it with
`play <file> from=free_roam` after `boot-to-roam.pad`'s steps, and reports how
far the car strayed from the recorded positions (`max_dev`); played back with
`--mcla_fixed_dt=120` it advances the game exactly as at the recording's cap.

### How repeatable (2026-10-14, RTX 5080, alternating runs after a warm-up)

- `drive-spawn.pad` as a timedemo (`--fixed-dt 60 --intro fast`, uncapped):
  the drive segment's median frame time varied 1.3-2.5% between runs (Native
  and 3x, both APIs) and every run's path stayed within 0.4-4.9 m of the first
  run's, including across APIs. The map zoom-out's median varied 0.5-0.7%
  (8% once on Vulkan at 3x).
- `boot-to-roam.pad` with the normal intro: the zoom-out took 6.4-7.2 s in
  every run. Uncapped, its frame-time median is not a good measure: about a
  quarter of uncapped swaps in the city carry no game update, and the normal
  intro's pacing holds each of those to 30 per second (33 ms), so medians mix
  two populations (spread up to 21%). At a 120 cap there are none.
- A recorded drive replayed within 0.06-0.8 m of the recording for its first
  3.5 s. Traffic is not the same from run to run: in the recording run the car
  stopped against something after 4.5 s, the replays drove on, and `max_dev`
  (70 m) flagged it.

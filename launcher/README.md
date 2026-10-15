# Midnight Club: Los Angeles launcher

Part of this fork (see `..\FORK.md`); Windows only. Desktop shortcut →
`launcher\app\MclaLauncher.exe`. On start it fetches the fork
([RenanQueiroz/mcla-recomp](https://github.com/RenanQueiroz/mcla-recomp)), lists
any new commits and offers **Update and rebuild**; **Play** starts `mcla.exe`
with the options set under *Options*. It also says how many commits upstream
([holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp)) has that
the fork has not taken yet: the fork takes them only when asked to.

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
in `docs\known-issues.md` (game) or `patches\README.md` (SDK).

## Options the launcher always passes

The launcher passes every game option explicitly (`--resolution_scale=1` for Native,
`--vsync=false` with every frame rate): the in-game menu saves what is in effect to
`mcla.toml`, which would otherwise win over an option left off the command line.
`--vsync=false` has to be on the command line: set from the game, it reaches only
mcla.exe's copy of the setting, and the GPU plugin keeps vsync on (60 FPS cap).

*Anti-aliasing* sets the game's 2× MSAA (`--mcla_msaa`). Off, a 3× frame took
18 to 25% less time on the title screen (13.2 → 9.9 ms in the night city view,
12.6 → 10.2 ms in Hollywood; about half the draws), for slightly more jagged thin
edges such as pylons and billboard frames; the dithered foliage looks cleaner.
Like the settings below, it is left out when *More mcla.exe options* sets `mcla_msaa`.

*Graphics API* picks the GPU plugin's backend (`--mcla_gpu_backend=d3d12` or
`vulkan`; "Game setting" passes nothing, and the plugin then tries Direct3D 12
first). Every setting and SDK patch here works on both; at native resolution
Vulkan is limited by its GPU command thread, which costs about 1.8 times as much
per draw as on D3D12 on this machine (`dev\vulkan-2026-10-14.md`). Left out when
*More mcla.exe options* sets `mcla_gpu_backend`.

It also always passes SDK settings that measured faster (in-run A/B; the first
two are ideas from [ForzaHorizon1-ReImagined](https://github.com/Dyotson/ForzaHorizon1-ReImagined)):

- `--clear_memory_page_state=false`: guest memory already uploaded to the GPU
  stays valid from one frame to the next instead of being copied again every
  frame (2.5 → 0.8 GB/s of uploads; 10 to 21% less GPU command thread time at
  Native, uncapped, depending on the scene). mcla-recomp's PS5 port runs this way too.
- `--execute_unclipped_draw_vs_on_cpu=true` (Xenia's default): draws with
  clipping off no longer claim the whole EDRAM, which saves render-target round
  trips (about 11% faster at 3×; no change at Native).
- `--shared_memory_hot_page_faults=4 --shared_memory_hot_page_ms=10000` (hot
  pages, as mcla-recomp's PS5 port runs): a page the game keeps rewriting is
  uploaded whenever a draw uses it instead of being write-protected again after
  every write fault. With the profiling notes' fast-path patch, write faults go
  from about 12,000 to about 200 a second and protection changes from about
  190,000 to 1,000-2,000; frames on the title screen at Native, uncapped, were
  about 9% shorter (6.87 against 7.57 ms, in-run switch every 20 s, both halves
  agree). Needs `rexglue-hot-pages-watched-ranges.patch`: without it, textures
  the CPU rewrites (the attract movie's colour planes) stopped updating.

Setting any of them under *More mcla.exe options* (e.g.
`--clear_memory_page_state=true` if geometry or textures ever look stale, or
`--shared_memory_hot_page_faults=0` to turn hot pages off) replaces the default
rather than adding to it: a boolean given twice adds up, and any other setting
given twice is an error for mcla.exe's command-line parser (CLI11 rejects a
repeated option by default).

Known interaction: fully uncapped at about 130 FPS or more with the intro at
"Normal", a swap can arrive without a pass through the frame timer, and
mcla-recomp's intro pacing holds it to 30 FPS (a ~28 ms hitch). Capped at 60 or
120 it does not happen; with the intro at "Fast" never.

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
  where its GPU command thread's time goes, the baseline for a Vulkan
  submission thread, validation, pacing and stress runs.

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
  sticks, waits in time or engine frames, `wait_until` / `press_until` a
  condition (`left_title`, `loading`, `roam`, or a guest memory word),
  screenshots, recording and playback. `--mcla_fixed_dt=N` makes every game
  update advance exactly 1/N s: uncapped, a timedemo.
- `dev\devrun.py` runs the game with a script **on a copy of the save data**:
  `prepare` snapshots `Documents\mcla` (career save, achievements) to
  `%LOCALAPPDATA%\mcla-dev\pristine`; every run restores
  `%LOCALAPPDATA%\mcla-dev\work` from it and passes `--user_data_root` and
  `--cache_root` there, backs up `mcla.toml` next to `mcla.exe` and puts it back
  after the run, and stops if the real save, achievements or `mcla.toml`
  changed. `run` runs one script, `bench` repeats one per configuration
  (alternating, e.g. `--configs d3d12:1,vulkan:1`) and reports each segment's
  median frame time, 1% low and run-to-run spread, `analyze` reads a run.
  `devrun.py prepare --refresh` takes a new snapshot after you have played on.
- `dev\scripts\`: `boot-to-roam.pad` (title, the map zoom-out loading, the
  city: the loading benchmark) and `mark-roam.pad` (for recordings).

The game ignores buttons during its own transitions (the title screen's camera
cuts, a menu closing), so scripts press until a condition holds rather than
once. Benchmarks run with `--mcla_intro=normal` (`devrun.py bench`'s default):
"fast" also lets the loading loops run unpaced.

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

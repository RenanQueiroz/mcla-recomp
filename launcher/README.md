# Midnight Club: Los Angeles launcher

Desktop shortcut → `app\MclaLauncher.exe`. On start it fetches
[mcla-recomp](https://github.com/holdmysocks/mcla-recomp), lists any new commits,
and offers **Update and rebuild**; **Play** starts `mcla.exe` with the options set
under *Options*.

```
..\                                  this folder's parent
├─ *.iso                             your disc image (only read until game\ is extracted)
├─ mcla-recomp\                      the git checkout; game\ and out\ live in here
└─ launcher\
   ├─ build-game.ps1                 update + build; the launcher runs this
   ├─ build-launcher.ps1             rebuild the launcher itself, recreate the shortcut
   ├─ src\                           launcher source (C#, Reactor on WinUI 3)
   ├─ app\                           the built launcher
   ├─ logs\                          one log per build the launcher ran
   ├─ settings.json                  launcher options
   └─ built-commit.txt               commit of the last successful build
```

## From a terminal

```powershell
.\build-game.ps1             # build (or finish building) what is checked out
.\build-game.ps1 -Update     # fast-forward to upstream, then build
.\build-game.ps1 -ResetSdk   # re-apply the SDK patches and rebuild the SDK
```

`build-game.ps1` wraps `mcla-recomp\scripts\make_pc.ps1` and adds what it leaves out:

- **New SDK patches.** `make_pc.ps1` patches the ReXGlue SDK only once, so when an
  update changes `patches\*.patch` the SDK checkout is reset and patched again
  (incremental SDK rebuild). If the SDK version itself changes, the SDK is re-cloned.
- **Local edits.** Updates use `git merge --ff-only --autostash`. Codegen rewrites
  `mcla_manifest.toml` and `generated\rexglue.cmake` with LF line endings, so they
  always look modified; autostash carries them across updates safely.
- **Python.** `make_pc.ps1` rejects any `python.exe` under `WindowsApps`, which is
  where the Python install manager puts its working one; the wrapper hands that one over.
- **Our own fixes to mcla-recomp** live in `patches\` and are applied before every
  build, taken off before an update and put back after it (a patch that stops
  applying is skipped with a warning):
  - `bike-gyro-continuous-time.patch` (cvar `mcla_bike_gyro_fix`, on): the
    motorcycle's lean filter (`mcBikeGyro::Update`, found by LARecomp) moves a
    fixed fraction per frame, doubled below 60 FPS, so it matches the console
    only at 30: at 120 it settles 1.3 to 1.8 times as fast, and a 60 FPS cap
    whose frames straddle 59.5 FPS flips between two filters frame to frame.
    Like mcla-recomp's camera fixes, the 30 FPS fraction is now applied by
    elapsed time. Cars use a different gyro that already steps by time.
  - `keep-timer-resolution.patch`: Windows 11 ignores the game's request for
    1 ms timer resolution while none of its windows can be seen (minimized,
    covered, or the display asleep), and the frame limiter's sleeps then
    overshoot by up to 15.6 ms (at a 60 cap, 56% of frames within 1 ms of the
    target instead of 98%). The game now opts out of that at start-up
    (`PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`) and logs whether it worked.
  - `on-demand-achievement-toast.patch`: the SDK registers its achievement toast
    for the whole session, and while it is registered the presenter repaints at
    the monitor's refresh rate, presenting each game frame several times (240 FPS
    in overlays at a 30-60 FPS game). This keeps the pop-ups but registers the
    toast only while one is on screen (about 5.5 s per achievement), and even
    then it is drawn with the game's own frames instead of repainted on its own
    (`imgui-dialog-repaint-on-demand.patch` below), so one frame is presented per
    game frame throughout. Repainting on its own, the toast took presents to
    240/s and frame pacing at a 60 cap to 65-83% of frames within 1 ms while it
    was shown; drawn with the game's frames it stays at 99-100%. Ported from
    ForzaHorizon1-ReImagined (MIT; the notice is in the `src\achievement_toast.h` it adds). It
    replaces `no-achievement-toast.patch`, which removed the toast. Unlocks are
    saved by the SDK either way, in `Documents\mcla\achievements\545407F8.toml`.
  - `pace-at-swap.patch`: the frame limiter waits when a frame is presented instead
    of before the game's update, so frames arrive evenly (std dev 3.1 ms → 0.4 ms
    at 60 FPS); `--mcla_pace_at_swap=false` restores the old behaviour.
- **Our own fixes to the ReXGlue SDK** live in `patches\sdk\` and go on top of
  the SDK patches `make_pc.ps1` applies. A newly applied one moves the SDK's
  `.mcla-patched` marker forward, which makes `make_pc.ps1` rebuild the SDK
  (about a minute) and then the game. On a fresh or reset SDK they are applied
  after `make_pc.ps1`'s first run, followed by a second run. They are applied in
  name order and some edit neighbouring lines, so which ones are in is recorded
  (name and hash) in `third_party\rexglue-sdk\.launcher-sdk-patches`; an SDK
  reset deletes that file together with the patches.
  - `inline-bit-helpers.patch`: `lzcnt`, `tzcnt` and `bit_scan_forward` defined
    inline in `rex/math.h` instead of out of line in `rexruntime.dll`, where every
    use on the GPU command thread was a cross-DLL call.
  - `gpu-regwrite-bulk.patch` (cvar `gpu_regwrite_bulk`, on): blocks of GPU
    registers are copied in bulk, and only the few registers with side effects
    go through the slow path (approach from xenia-canary). About 10% less GPU
    command thread time per draw, measured in-run against the old path.
  - `shared-memory-request-fast-path.patch` (cvar `shared_memory_request_fast_path`,
    on): a request whose pages are all valid already returns without batching,
    sorting or the global lock. About 2.5% less time per draw.
  - `perf-counters-batched.patch` (cvar `gpu_batch_perf_counters`, on): the GPU
    command thread's per-draw counters (draws, vertices, texture and pipeline
    lookups) add up locally and reach the shared counters once a frame, instead
    of an atomic call into `rexruntime.dll` per event. About 0.8% less per draw.
  - `timer-queue-blocking-wait.patch` (cvar `timer_queue_blocking_wait`, on): the
    timer thread sleeps until the next timer is due or a new one is queued
    instead of spinning, which kept about half a core busy; no frame-rate change.
    Also fixes the argument order of disruptorplus' blocking `wait_until`.
  - `imgui-dialog-repaint-on-demand.patch`: an ImGui dialog can say it needs no
    repaints of its own (`ImGuiDialog::RepaintsContinuously()`, true by default).
    While only such dialogs are shown, the drawer no longer has the presenter
    repaint at the monitor's refresh rate; they are drawn whenever a new game
    frame is painted. Used by the achievement toast above, whose code checks
    `REX_UI_IMGUI_DIALOG_REPAINT_ON_DEMAND`, so it still builds without this.
  - `threaded-submission-d3d12.patch` (cvars `d3d12_threaded_submission`, on, and
    `d3d12_threaded_submission_split_draws`, 1000): the GPU command thread only
    records each submission's commands; a second thread ("GPU Submission")
    replays them into the Direct3D 12 command list, submits it and signals its
    fence while the command thread goes on with the next draws. That replay,
    mostly driver time, was 20-40% of the command thread's time. Submissions are
    also ended every 1000 draws, so the replay starts early and little of it is
    left when a frame is presented; presenting still waits until the whole frame
    is in the queue, so latency does not grow. Design from ForzaHorizon1-ReImagined's
    Vulkan patch 0038. In-run A/B against the old path, Native uncapped, heavy
    title view: -15% time per draw (95 → 111 FPS), of which the 1000-draw split
    is about 7 points; at 3× -4.7% (the split is neutral there). Where the game
    thread rather than the command thread sets the pace (light views, a 60 cap
    the game holds anyway) it makes no frame-rate difference, but the command
    thread's own CPU time per frame drops 20-38%. Both cvars can change while
    the game runs; `--d3d12_threaded_submission=false` restores the old path.

  Tried and dropped (no measurable effect in in-run A/B): specialized system
  constant updates, sampler fetch-constant prefetch, reused texture request
  queues, skipping the clock in shared-memory requests, keeping bindings for
  fetch constants rewritten unchanged (ForzaHorizon1-ReImagined's patch 0032,
  -0.4%), and the PS5 hot-page write-watch settings
  (`--shared_memory_hot_page_faults=4`), which measured slightly worse.

  Known interaction: fully uncapped at about 130 FPS or more with the intro at
  "Normal", the faster GPU thread lets a swap arrive without a pass through the
  frame timer, and mcla-recomp's intro pacing holds it to 30 FPS (a ~28 ms hitch).
  Capped at 60 or 120 it does not happen; with the intro at "Fast" never.

The launcher passes every game option explicitly (`--resolution_scale=1` for Native,
`--vsync=false` with every frame rate): the in-game menu saves what is in effect to
`mcla.toml`, which would otherwise win over an option left off the command line.

It also always passes two SDK settings that measured faster (in-run A/B, ideas
from [ForzaHorizon1-ReImagined](https://github.com/Dyotson/ForzaHorizon1-ReImagined)):

- `--clear_memory_page_state=false`: guest memory already uploaded to the GPU
  stays valid from one frame to the next instead of being copied again every
  frame (2.5 → 0.8 GB/s of uploads; about 21% less GPU command thread time at
  Native, uncapped). mcla-recomp's PS5 port runs this way too.
- `--execute_unclipped_draw_vs_on_cpu=true` (Xenia's default): draws with
  clipping off no longer claim the whole EDRAM, which saves render-target round
  trips (about 11% faster at 3×; no change at Native).

Setting either one under *More mcla.exe options* (e.g.
`--clear_memory_page_state=true` if geometry or textures ever look stale)
replaces the default rather than adding to it.

## Rebuilding the launcher

Needs the .NET 10 SDK (`winget install Microsoft.DotNet.SDK.10`). Close the
launcher, then run `.\build-launcher.ps1`.

Package versions float: each build takes the newest Reactor preview and the newest
stable Windows App SDK 2.x. Reactor is pre-1.0 and a new preview can change its API,
so if a rebuild suddenly fails to compile, check what changed in
`%USERPROFILE%\.nuget\packages\microsoft.ui.reactor\<version>\agentkit\reactor.api.txt`
(the index for the version actually used; Reactor's GitHub main runs ahead of it).

The icon is `src\launcher.ico`, converted from `..\mcla_icon.png`.

## History and measurement tools

This folder is a local git repository (nothing is pushed anywhere): every kept
change to the patches, scripts or launcher is a commit, so any earlier state can
be restored with `git log` and `git checkout <commit> -- <path>`. Built output,
logs, `settings.json` and `built-commit.txt` are not tracked.

`dev\` holds what the measurements above were made with; `build-game.ps1` never
reads it:

- `ab-harness.patch` (mcla-recomp, `src\perf_options.cpp`): flips cvars in-run
  every `MCLA_AB_SECONDS` (default 4) seconds, e.g.
  `MCLA_AB=set:gpu_regwrite_bulk=on/off`, so an A/B compares the same scenes.
  Run uncapped with `--mcla_intro=fast` and `REX_PERF_LOG_CSV`, and leave out
  the first 20 s and the attract video (about 120-131 s after launch).
- `draw-cost.py`: fits frame time = fixed + per draw x draws over a run's
  `REX_PERF_LOG_CSV` output.
- `dropped-patches\`: patches that were measured and not kept.

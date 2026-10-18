# MCLA: a fork of mcla-recomp

A fork of [holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp), the static recompilation of *Midnight Club: Los Angeles Complete Edition* (Xbox 360, title 545407F8) to native code for Windows, Linux and jailbroken PS5 consoles. On top of upstream's `main` it adds performance work on both Windows renderers, Direct3D 12 and Vulkan, so that higher resolutions and frame rates hold; a Windows launcher that updates, builds and starts the game; and several game fixes. The changes meant for upstream are offered there as pull requests over time.

**This repository contains no game code, assets or keys.** You supply your own legally obtained copy; a script extracts what is needed on your machine, and the recompiled code is generated locally and never committed. What you build is for you, not for sharing.

## Testing so far

Everything this fork adds was built and measured on **Windows only**, with an **NVIDIA RTX 5080** and a **Ryzen 7 9800X3D**, on both Direct3D 12 and Vulkan. None of it has been built or run on AMD or Intel graphics, on Linux or on the PS5 yet. If you try it on other hardware, please report how it went in the [fork's issues](https://github.com/RenanQueiroz/mcla-recomp/issues), whether it works or not.

## Status

| Target | State in this fork |
|---|---|
| Windows, Direct3D 12 | Plays: career, races, garage and upgrades, with audio and a controller. 30 to 120 FPS or uncapped, at native resolution (720p), 2× or 3× (4K). |
| Windows, Vulkan | Runs with the same settings, through the same scripted runs as D3D12: the map zoom-out, free roam, a drive and a race. Not yet played through by hand. (Upstream lists it as reaching the title screen.) |
| Linux, Vulkan | Upstream's build plays: free roam holds 30 FPS on an Intel Core Ultra 5 238V laptop (Mesa ANV), tested by one user. The fork's SDK patches apply on top, but have not been built or run there. |
| PS5 (jailbroken) | Upstream's build plays at a steady 30 FPS on a PS5 Pro (firmware 13.42), 20 to 25 in the zoomed-out map, and was installed and played on a PS5 Slim (firmware 12.70). The fork's `make_ps5.sh` also applies the fork's SDK patches, which have not been built or run on a console. |

On the test machine, with the launcher's settings: at native resolution the game's own thread sets the pace, at about 6.8 ms a frame (about 145 FPS uncapped in free roam). At 3× (4K) a 120 FPS cap holds in free roam and while driving in the city, with 93% of frames within 1 ms of the target on D3D12 and 97% on Vulkan (99% on both standing in the city by day since single-tile rendering). Races at 3× run at a median of 8.1 to 8.5 ms a frame on D3D12 and 9.3 to 9.6 ms on Vulkan in the race benchmark (`launcher/dev/scripts/race.pad`, uncapped, the release build, two runs each); with the scene in two tiles 9.7 to 10.1 and 10.9 to 11.1 ms, where the runs of 2026-10-15 gave 9.3 and 10.8 (the benchmark's runs spread by up to 8%). Method and per-change figures: [patches/README.md](patches/README.md) and `launcher/dev/single-tile-edram-2026-10-16.md`.

Car reflections are fixed on every renderer (upstream's fix: cars no longer come out white with a glow at night):

![The same spot before and after the fix, at night and at noon](docs/images/reflection-fix-before-after.webp)

## Which release of the game

Only **Midnight Club: Los Angeles Complete Edition, USA/Europe, for Xbox 360** (title id `545407F8`), as a disc image (`.iso`). The fixes in `config/` are addresses in that release's executable; another region or revision would need them checked again.

## How it works

The game's PowerPC code is translated to C++ ahead of time by the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (BSD-3, derived from Xenia), which also provides the Xbox 360 kernel, GPU and audio reimplementation. This repository holds the project manifest, function hints, a small host application, patches to the SDK and the build scripts. The fork adds its own SDK patches to `patches/`, changes to the host application in `src/`, and the launcher in `launcher/`.

## Windows

You need Windows 10 or 11 (x86-64) with a Direct3D 12 graphics card, and about 20 GB of disk. The build checks for Git, CMake, Ninja, Python 3, Visual Studio Build Tools 2022 (C++ workload) and the project's own Clang 20, and offers to install what is missing (answer `y`, or add `-InstallTools`); the Build Tools ask for administrator rights. It also looks for the Vulkan SDK (`winget install KhronosGroup.VulkanSDK`, administrator rights too), which only the FSR anti-aliasing option needs on Vulkan: without it FSR is built for Direct3D 12 only, and the build goes on. FSR's runtime is built from the FidelityFX SDK's source, which the first build fetches from GitHub; without the network the game builds without FSR. The launcher also needs the .NET 10 SDK (`winget install Microsoft.DotNet.SDK.10`).

### With the launcher (recommended)

In PowerShell:

```powershell
git clone https://github.com/RenanQueiroz/mcla-recomp.git
cd mcla-recomp
.\launcher\build-game.ps1 -Iso C:\path\to\your.iso
.\launcher\build-launcher.ps1
```

`build-game.ps1` runs `scripts\make_pc.ps1`, which clones the ReXGlue SDK v0.10.0 and applies upstream's and the fork's patches, builds it (the long step, once), extracts your game into `game\`, recompiles it and builds `out\build\win-amd64-release\mcla.exe`. Without `-Iso` it takes the first `.iso` in the folder that holds `mcla-recomp`. Each step is skipped when its result is already there, so after a failure you run the same command again. Logs are in `out\logs`. `build-launcher.ps1` builds the launcher into `launcher\app` and puts a **Midnight Club Los Angeles** shortcut on the desktop.

From then on the shortcut is the way in. The launcher opens on the game's settings, and **Play** starts `mcla.exe`. When it opens it checks this fork on GitHub; if there are new commits it lists them under *What's new* and offers **Update and rebuild**. Commits that change only the launcher or the documentation don't need a game rebuild; when an update changed the launcher itself, it offers **Restart to update the launcher**. The **More** menu has *Rebuild the game*, *Re-patch and rebuild the SDK*, *Run the build in a terminal*, *Prepare shaders*, the game folder and the build logs, and says how many of upstream's commits the fork hasn't taken yet. Keep the repository where it was built: the game data is read from `game\`.

The launcher also **prepares shaders**: after every build and update, and before Play whenever the game's record of its shader storage says the graphics driver was updated, the card, the API or the resolution changed, the shader pack changed, or the shader cache was deleted or reset (the status line says why, counts the shaders and pipelines, and the game starts when it's done). It runs `mcla.exe --mcla_prepare_shaders=true` with the game's settings: it adds what the game's shader storage lacks of the fork's shader pack, rebuilding those shaders from your own game files, has the graphics driver compile every stored pipeline for your API and resolution, and quits. Places, cars and races the pack covers then don't compile shaders when you first meet them (no objects missing for a moment). The game itself also adds what its storage lacks of the pack at every start, so a storage that was deleted or reset fills again even without the launcher. The pack holds no game data, only fingerprints and the pipeline descriptions the storage keeps anyway ([KNOWN-ISSUES.md](KNOWN-ISSUES.md#shaders-prepared-before-the-first-start-and-kept-prepared-2026-10-17)). **More** › *Prepare shaders* runs it by hand, and also tries again a pack whose import was turned off after it stopped the game's start twice.

The launcher's settings are the game's own, kept in `mcla.toml` next to `mcla.exe`. The Settings tab of the game's pause menu changes the same ones, so each shows what the other set. A change in the launcher is written to the file at once. While the game runs, the launcher's settings are greyed out: change them in the pause menu then, and the launcher shows them again once the game has closed.

| Setting | Choices | In `mcla.toml` | In the pause menu |
|---|---|---|---|
| Resolution | Native (720p), 2× (1440p), 3× (2160p, 4K) | `resolution_scale` = `1`, `2` or `3` | DISPLAY › RESOLUTION (next start) |
| Frame rate | 30, 40, 45, 50, 60, 120 FPS, Uncapped | `mcla_fps` (0 for uncapped) | DISPLAY › FPS TARGET |
| Display | Fullscreen, Windowed | `fullscreen` = `true` or `false` | DISPLAY › FULLSCREEN |
| Display gamma | TV (as on the console), PC monitor (sRGB, brighter shadows) | `mcla_display_gamma` = `tv` or `pc` | DISPLAY › DISPLAY GAMMA |
| HDR | Off, Auto (when Windows HDR is on; the default), On | `hdr` = `off`, `auto` or `on` | DISPLAY › HDR |
| HDR peak brightness | Auto (what the display reports), or 200 to 4,000 nits in steps of 50 | `hdr_peak_nits` (0 for auto) | DISPLAY › PEAK BRIGHTNESS |
| HDR paper white | Auto (Windows' SDR content brightness), or 80 to 480 nits in steps of 10 | `hdr_paper_white_nits` (0 for auto) | DISPLAY › PAPER WHITE |
| HDR highlights | Normal, Bright, Brighter (the default), Brightest | `mcla_hdr_highlights` = `1`, `1.5`, `2` or `3` | DISPLAY › HIGHLIGHTS |
| Anti-aliasing | MSAA 2× (as on the console; the default), FSR 3.1 native AA (AMD FSR at native resolution: steadiest, a little softer), Off (2 to 5% faster at 3× than MSAA) | `mcla_anti_aliasing` = `msaa`, `fsr` or `off` (an older `mcla_msaa = false` still means off while it isn't set) | PERFORMANCE › ANTI ALIASING |
| FSR sharpness | Off, 0.2 (the default), 0.4, 0.6, 0.8, 1.0; FSR only | `temporal_aa_sharpness` | PERFORMANCE › FSR SHARPNESS |
| Single tile rendering | With MSAA: On (the scene drawn once instead of in two tiles: 9 to 20% faster at 3×), Off (two tiles, as on the console); no difference with FSR or Off | `mcla_single_tile` = `true` or `false` | PERFORMANCE › SINGLE TILE RENDERING |
| HUD updates | 30 per second (as on the console), 60 per second (the default), 120 per second, Every frame | `mcla_ui_hud_rate` = `30`, `60`, `120` or `0` | DISPLAY › HUD UPDATES |
| Graphics API | Automatic (Direct3D 12 first), Direct3D 12, Vulkan | `mcla_gpu_backend` = `any`, `d3d12` or `vulkan` | DISPLAY › GRAPHICS API (next start) |
| Intro | Normal, Fast, Skip (hides the title logo) | `mcla_intro` = `normal`, `fast` or `skip` | CONTROLS › INTRO (next start) |
| Button prompts | Follow the controller, Xbox, PlayStation | `mcla_button_prompts` = `auto`, `xbox` or `playstation` | CONTROLS › BUTTON PROMPTS |

*Display gamma* is the ramp the picture goes through on its way to the screen, as the console's display hardware applied it. The game writes sRGB-encoded colour; *TV*, the default, is the console's ramp for an HDTV (BT.709), which on a PC monitor makes the shadows darker than the game drew them (8-bit levels 64, 128 and 192 show as 48, 115 and 185). *PC monitor* leaves the game's output as it is, as an sRGB monitor expects. It changes during play, from the launcher or the pause menu ([KNOWN-ISSUES.md](KNOWN-ISSUES.md#display-gamma-for-pc-monitors-2026-10-16)).

*HDR* shows the game in HDR on a display in HDR mode (Windows' *Use HDR*): the game's HUD, menus and movies at the SDR level, as on the desktop, and its scene from its own tone map's inputs: the same as in SDR up to 60% of the game's white, never darker than SDR from there to the game's white, and brighter above it (lights at night, the sun, a dusk sky, headlights), the brightest highlights reaching about 90% of the peak setting. The default on Windows is *Auto*, which turns it on whenever Windows has HDR on for the display (with HDR off in Windows the game stays SDR as before); the look was signed off on an ASUS ROG Swift PG32UCDP (WOLED, Gaming HDR mode, Uniform Brightness on), 2026-10-16. *Off* keeps SDR always. *Peak brightness* and *paper white* default to what Windows reports: the peak the Windows HDR Calibration app measured, and the *SDR content brightness* slider, where the game's white lands; set the peak by hand if your display's HDR mode clips earlier (the launcher shows what Windows reports; `--hdr_test_pattern=true` shows bars to find where your display stops getting brighter). In the launcher each is a slider with an *Auto* box; in the pause menu they step by 50 and 10 nits, holding left or right goes faster, and AUTO comes first. The peak is never set below paper white: a step that would cross it stops at the last one that doesn't. A value typed into `mcla.toml` between the steps is kept and shown as it is. *Highlights* (Brighter by default) sets how far above the game's white bright things go. DISPLAY GAMMA keeps its meaning in HDR. All of them change during play ([KNOWN-ISSUES.md](KNOWN-ISSUES.md#hdr-output-2026-10-16)).

The 40 to 50 FPS caps are for G-Sync and FreeSync displays: a cap the game holds even in its heaviest scenes gives constant frame times. The game turns vsync off by itself (the pause menu's DISPLAY › VSYNC turns it on): with it on, the GPU plugin's emulated 60 Hz vblank caps the game at 60 and paces it unevenly.

Under *Advanced* are **More mcla.exe options** (any other options, for example `--log_file=run.log`; they are the only options the launcher passes, and one set there wins over `mcla.toml` and the pause menu at every start, also after the pause menu's RESTART GAME, which starts the game with the same options: `--resolution_scale` there overrides DISPLAY › RESOLUTION) and three checkboxes: check for updates when the launcher opens (on), close the launcher when the game starts (on), and start the game right away when there's nothing to update (off; a short countdown you can cancel).

Launchers before this one passed their choices on the command line at every start, which beat what the pause menu saved. The first time this one opens, it writes the choices that were forcing a value into `mcla.toml` (with `vsync = false`, which came with every frame rate), so nothing changes for you, after keeping a copy of the file as `mcla.toml.before-launcher-settings` next to it. That happens once: the copy marks it as done, and `settings.json` no longer keeps the old choices. If the game is running then, it waits until the game has closed ([launcher/README.md](launcher/README.md#the-games-settings)).

On PC the game also uses four SDK settings that measured faster on this game as its own defaults (`src/mcla_app.cpp`): `clear_memory_page_state=false`, `execute_unclipped_draw_vs_on_cpu=true`, and hot pages, `shared_memory_hot_page_faults=4` with `shared_memory_hot_page_ms=10000`, as upstream's PS5 build runs. The launcher used to pass them. A fifth, `edram_tiles=4096`, gives the GPU the larger EDRAM single-tile rendering needs. An option on the command line or a line in `mcla.toml` replaces each of them.

### Without the launcher

```powershell
git clone https://github.com/RenanQueiroz/mcla-recomp.git
cd mcla-recomp
.\scripts\make_pc.ps1 -Iso C:\path\to\your.iso
```

This is upstream's one command, with the fork's patches. It leaves a **Midnight Club Los Angeles** shortcut in the repository folder; double-click it, or `mcla.exe` itself. An Xbox or PlayStation controller works.

Options go after the program name, for example `mcla.exe --fullscreen=true --resolution_scale=2 --mcla_fps=120`; they win over `mcla.toml`. `--mcla_fps` takes the frame-rate target, or 0 for uncapped; the game turns vsync off by itself unless the pause menu's VSYNC is on.

The pause menu's Settings tab has three extra submenus. DISPLAY has the frame-rate target, HUD updates, vsync, fullscreen, resolution, graphics API, display gamma, motion blur and depth of field; PERFORMANCE has single-tile rendering, the detail options and anti-aliasing; CONTROLS has button prompts, intro speed and hiding the mouse pointer. What you change there is saved to `mcla.toml` next to `mcla.exe`, merged into what the file already holds; it is also saved when the window is closed with a submenu open.

Rows marked *(NEXT START)* take effect when the game next starts, and say *(RESTART NEEDED)* once changed. **RESTART GAME** (Windows; press it twice) saves, starts the game again with the same options and closes this one; the new one waits for the old one to have closed (if it hasn't within 30 seconds, it says so and doesn't start). As with **QUIT GAME**, progress since the last save is lost. QUIT GAME, RESTART GAME and the window's close button let a career save that is being written finish first, for up to 10 seconds.

### Updating

The launcher's **Update and rebuild** does it all. From a terminal, `.\launcher\build-game.ps1 -Update` does the same: it fast-forwards to the fork's latest commit, keeping local changes (codegen rewrites `mcla_manifest.toml` and `generated\rexglue.cmake` with other line endings, so they always look modified, and `git pull` can refuse to update them); it patches the SDK afresh when the update changed anything in `patches\`, and builds.

By hand: `git pull`, then `.\scripts\make_pc.ps1` again. It adds new SDK patches to an SDK patched earlier, and records each patch it applies: if a patch has changed since it was applied to your SDK, the build stops and names it. Then run `.\launcher\build-game.ps1 -ResetSdk`: it resets the SDK checkout, patches it again and rebuilds it (incrementally), then builds the game.

### If something goes wrong

| If | Then |
|---|---|
| PowerShell says `running scripts is disabled on this system` | `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`, then run the script again. |
| The SDK build fails with `'crtversion.h' file not found` | Another Clang on your path was used. Update and run `make_pc.ps1` again: it now uses only its own. |
| `codegen failed` | `game\default.xex` is missing or not the supported release; see the end of `out\codegen.log`. |
| After an update, the build says a patch has changed since it was applied, is in the SDK without a record, or does not apply | `.\launcher\build-game.ps1 -ResetSdk`, or the launcher's **More** → *Re-patch and rebuild the SDK*. |
| `no .NET 10 SDK found` | `winget install Microsoft.DotNet.SDK.10`, then `.\launcher\build-launcher.ps1` again. |
| The game stays at 60 FPS or paces unevenly | Turn VSYNC off in the pause menu's DISPLAY (or remove `vsync = true` from `mcla.toml`). |
| The game doesn't start after you added options under *More mcla.exe options* | Check that none is given twice: an option given twice is an error. |
| Geometry or textures look stale | Try `--clear_memory_page_state=true`, then the switches in [Turning a change off](#turning-a-change-off), and report what made the difference. |
| The launcher says *Shaders weren't prepared* | The game plays as before, compiling shaders as it meets them. The message says why, and `launcher\logs\prepare-shaders-*.log` (with the game's own log, `prepare-shaders-game.log`) has the details. *In use by a running game*: it's tried again at the next Play. *This default.xex is not the one the shader pack was made for*: your game isn't [the supported release](#which-release-of-the-game). |
| The game closes by itself | Start it with `--log_file=run.log --log_level=info` (the file is written next to `mcla.exe`; **More** → *Open the game folder*) and open an issue with the end of that file (see [Missed functions](#missed-functions) for where). |

## What the fork changes

Each change is kept separate, so that it can be offered upstream as a pull request of its own. The SDK patches and their measurements are in [patches/README.md](patches/README.md); the game changes, how each was checked and what is not verified, in [KNOWN-ISSUES.md](KNOWN-ISSUES.md); the list and the order of the patches in [FORK.md](FORK.md). The figures are from the test machine with the launcher's settings; 3× is 4K.

**Performance, both APIs**

| Change | Effect |
|---|---|
| Direct3D 9-style clear rectangles done as host render target clears, not copies between MSAA modes | 13 to 32% shorter frames at 3× |
| Single-tile rendering: with anti-aliasing on, the scene is drawn once instead of in the console's two tiles, in a 4,096-tile EDRAM the game asks the SDK for on PC | 9 to 11% shorter frames at 3× in the city, 15 to 20% in a race; 28 to 35% less GPU command thread time; a 120 FPS cap at 3× in the city 93-94% to 99% of frames within 1 ms |
| Resolves read the render target directly instead of dumping it into the EDRAM buffer first | 7 to 12% shorter frames at 3× in the city, 4 to 9% in a race; with it a 120 FPS cap holds at 3× in the city |
| The EDRAM buffer path reads the right 4x MSAA depth samples at odd resolution scales | no more columns 4 pixels wide, 120 apart at 3×, in the daylight depth |
| Resolved textures decoded straight into the texture, not through a buffer and a copy | 4 to 6% shorter frames at 3× |
| Frames no longer held while new pipelines compile | with nothing cached, worst frame 0.4 to 0.19 s on D3D12, 1.9 to 0.25 s on Vulkan |
| Shaders prepared before the first start and kept prepared: the game adds what its storage lacks of the fork's shader pack (fingerprints only, rebuilt from your game files) at every start, and the launcher compiles every pipeline after builds and before Play when the driver, the settings or the storage changed | with the driver's cache cold, no pipeline compiled while playing the race benchmark instead of 955 to 975 (D3D12) and 743 to 759 (Vulkan); map zoom-out 1% low 24 to 26 FPS before, 29 to 30 after (D3D12), 14 to 24 before, 29 to 31 after (Vulkan) |
| GPU register writes in bulk | about 10% less GPU command thread time per draw at native |
| Shared-memory requests whose pages are valid return at once, also with hot pages | 2.4% less GPU command thread time per draw; about 17% with hot pages |
| Hot pages on by default on PC, with watched pages kept out of the hot set | about 9% shorter frames on the title screen at native; the attract movie's colours stay right |
| PC defaults `clear_memory_page_state=false` and `execute_unclipped_draw_vs_on_cpu=true` (the launcher passed them before) | older estimates, not dated measurements: 10 to 21% less GPU command thread time at native, about 11% faster at 3× (to be measured again) |
| Batched GPU counters; inline bit helpers | fewer calls across DLLs on the GPU command thread |
| The timer queue thread sleeps instead of spinning | about half a core less CPU; no change in frame rate |

**Direct3D 12**

| Change | Effect |
|---|---|
| Command lists replayed and submitted on their own thread | 15% less GPU command thread time per draw at native |
| Textures placed in shared 256 MB heaps, not one allocation each | no more stutters in the map zoom-out and races: race 1% low 50 to 96 FPS at native, 41 to 61 at 3× |

**Vulkan**

| Change | Effect |
|---|---|
| Shared memory uploads recorded before the render pass instead of ending it | free roam 19 to 27% faster |
| Texture loads copied into their images by a compute shader (NVIDIA only by default) | 19% shorter frames at 3× |
| Command buffers recorded and submitted on their own thread | 5 to 7% where the GPU command thread limits |
| Draws test for memory exports before a cvar lookup | 6% less GPU command thread time per draw |
| The shader storage translated in parallel at start-up | 0.3 to 0.05 s of every start |
| An optional pipeline cache file (off) | for drivers without a disk cache of their own |
| The per-frame counters (draws and the rest) counted on Vulkan too | |

**HDR output, both APIs**

| Change | Effect |
|---|---|
| HDR10 output: the swap chain, the display's levels from Windows, HDR10 metadata, the presenter's own UI as SDR on top | the game at the SDR level in HDR; DISPLAY GAMMA, captures and the ImGui windows work as in SDR |
| The game's HUD and menus kept apart from the scene, and its frame put back for what it reads | the UI at the SDR level over an HDR scene, translucent panels blended as the game blends them |
| The scene from the game's tone map inputs, with a shoulder from 60% of its white instead of its clip | up to 0.8 to 1.2 thousand nits on a 1300-nit display (the brightest highlights at about 90% of the peak setting); the same as SDR up to 60% of the game's white and never darker than SDR up to its white; 0.5 ms of GPU time at 3x |

**Game fixes**

| Change | Effect |
|---|---|
| The frame limiter waits at the swap, not before the update | frame-time standard deviation at a 60 cap 3.1 to 0.38 ms |
| The 1 ms timer kept while the window is hidden | no 16 ms frames when minimized, covered or with the display asleep |
| The achievement toast registered only while it shows | no repaints at the monitor's rate (240 a second on a 240 Hz display) |
| The SDK's overlays (F4 settings, console, pop-ups) put over each game frame instead of presented by the UI thread | the game keeps its frame rate with them open: 146 a second instead of 60 to 113 (native, uncapped); they refresh at the game's rate |
| The motorcycle's lean eased by elapsed time | matches the console at any frame rate (not verified by riding) |
| The mouse pointer hidden during play | goes on controller input or after about 3 s still; back when the mouse moves |
| Loading-screen pacing | uncapped free roam at native: mean 81 to 145 FPS; the fast intro speeds up only the start-up logos |
| The pause menu's settings: only what you change is saved, merged into `mcla.toml`, also when the window is closed; the FPS list has 40, 45 and 50; resolution, graphics API and hiding the pointer added; anti-aliasing switches during play; restart-only rows say so, and *(RESTART NEEDED)* once changed; RESTART GAME; the tabs come back after a submenu | command-line options and paths no longer end up in the file, and a file with an old-style path loads again (`patches/rexglue-cvar-toml-strings.patch`); QUIT GAME closes the game (it left it running behind a window that no longer answered); closing waits for a career save being written |
| A display gamma setting: the console's TV ramp (the default) or sRGB for PC monitors, switched during play | on a PC monitor the shadows as the game drew them: a night scene's average brightness 46 to 60 of 255 |
| The launcher edits `mcla.toml` instead of passing options | the launcher and the pause menu show and change the same settings; a choice made in the game sticks |
| Menus, pop-ups and map panels at the console's pace above 30 FPS; the HUD at 60 a second by default, or 30 (the console's pace), 120 or every frame | the pause menu opens in 0.88 s at 120 FPS (0.93 s at 30; 0.18 s before); the song title under the speedometer scrolls at its original speed |

Upstream's own fixes, such as the car reflections and the camera-flight warp that never ended, are in the fork too.

**Tools (fork only, never offered upstream)**

- `launcher/`: the Windows launcher above ([launcher/README.md](launcher/README.md)).
- `launcher/dev/`: what the measurements were made with: in-run A/B switching, GPU timestamps and other instrumentation as SDK patches, the analysis scripts, the dropped patches and the dated notes. Nothing in the build uses it.
- `src/dev_pad.cpp`: scripted controller input for unattended runs and benchmarks (`--mcla_pad_script`, `--mcla_pad_pipe` and the others), driven by `launcher/dev/devrun.py` on a copy of the save data. Off unless its options are set ([launcher/README.md](launcher/README.md#scripted-controller-input-unattended-runs)).

## Turning a change off

Every change with a setting can be turned off without rebuilding: type the option under the launcher's *Advanced* → **More mcla.exe options**, or pass it to `mcla.exe`. If a problem goes away with a change off, please say which option did it in the [fork's issues](https://github.com/RenanQueiroz/mcla-recomp/issues).

| Change | Option | Notes |
|---|---|---|
| Hot pages (PC default) | `--shared_memory_hot_page_faults=0` | The game's PC default is 4, with `shared_memory_hot_page_ms=10000`; the SDK's is 0. |
| Uploaded memory kept valid across frames (PC default) | `--clear_memory_page_state=true` | The SDK's default. |
| Unclipped draws' vertex shaders on the CPU (PC default) | `--execute_unclipped_draw_vs_on_cpu=false` | The SDK's default. |
| Clear rectangles as host clears | `--clear_rectangle_draws=false` | |
| Direct resolves | `--direct_host_resolve=false` | |
| Single-tile rendering | `--mcla_single_tile=false` | Or the launcher's *Single tile rendering*, or PERFORMANCE › SINGLE TILE RENDERING in the pause menu, which takes effect at once. |
| The 4,096-tile EDRAM (PC and PS5 default) | `--edram_tiles=2048` | The SDK's default, the console's size; single-tile rendering then has no room and is off (its pause menu row says UNAVAILABLE). On the PS5, `edram_tiles = 2048` in `/data/mcla/mcla.toml`. |
| Resolved textures decoded straight in | `--texture_load_direct=false` | Takes effect at once; turning it back on needs a restart. |
| No frame wait for new pipelines | `--async_pipeline_end_of_frame_wait=true` | Restores the wait. The old behaviour in full also needs `--async_pipeline_wait_budget_ms=0 --vulkan_async_skip_incomplete_frames_max_ms=0 --vulkan_async_placeholder_pipelines=true`. |
| Register writes in bulk | `--gpu_regwrite_bulk=false` | |
| Shared-memory request fast path | `--shared_memory_request_fast_path=false` | With hot pages on, `--shared_memory_request_fast_path_with_hot_pages=false` turns off only that part. |
| Batched GPU counters | `--gpu_batch_perf_counters=false` | |
| Timer queue sleeps | `--timer_queue_blocking_wait=false` | |
| D3D12 threaded submission | `--d3d12_threaded_submission=false` | `--d3d12_threaded_submission_split_draws` (default 1000, 0 for never) splits long submissions. |
| D3D12 placed texture heaps | `--d3d12_texture_heaps=false` | Textures created from then on are separate allocations again. |
| Vulkan uploads before the render pass | `--vulkan_hoist_shared_memory_uploads=false` | |
| Vulkan threaded submission | `--vulkan_threaded_submission=false` | `--vulkan_threaded_submission_split_draws`, as on D3D12. |
| Vulkan texture loads by compute | `--vulkan_texture_load_storage_copy=off` | Default `auto`, on with NVIDIA only; `on` tries it on other GPUs, and with it the direct texture loads on Vulkan. |
| Vulkan shader storage in parallel | `--vulkan_pipeline_storage_translation_threads=1` | Default -1, as many threads as pipeline creation uses. |
| Vulkan pipeline cache file | `--vulkan_pipeline_cache_file=true` turns it on | Off by default. |
| Frame pacing at the swap | `--mcla_pace_at_swap=false` | |
| Motorcycle lean | `--mcla_bike_gyro_fix=false` | |
| Menus at the console's pace | `--mcla_ui_timing_fix=false` | |
| HUD at a rate of its own | `--mcla_ui_hud_rate=0` advances it with every frame | Default 60 a second; also 30 (the console's pace) or 120. The launcher's *HUD updates*, and DISPLAY › HUD UPDATES in the pause menu. |
| Mouse pointer hidden during play | `--mcla_hide_cursor=false` | Or CONTROLS › HIDE MOUSE POINTER in the pause menu, which takes effect at once. |
| Camera-flight warp fix (upstream's) | `--mcla_warp_fix=false` | |
| HDR output (Windows default `auto`) | `--hdr=off` | Or `hdr = "off"` in `mcla.toml`, the launcher's *HDR*, or DISPLAY › HDR in the pause menu, during play. It only acts while Windows has HDR on for the display. |
| The HUD and menus apart from the scene in HDR | `--hdr_ui_layer=false` | The whole frame as SDR content in HDR. |
| The scene in HDR | `--mcla_hdr_scene=false` | The scene as the game's 8-bit picture, under the separate UI. |
| The shader pack's import at every start | `--mcla_shader_pack_auto=false` | Or `mcla_shader_pack_auto = false` in `mcla.toml`. The storage then keeps only what the game compiled itself; the launcher's *Prepare shaders* still adds the pack. |
| Render markers | `--mcla_render_markers=false` | HDR then shows the whole frame as SDR. |

Without a setting: the timer kept at 1 ms while the window is hidden, the achievement toast and its repaints, the loading-screen pacing, the inline bit helpers, Vulkan's memory export check and per-frame counters, watched pages kept out of the hot set (it matters only with hot pages on), and the 4x MSAA depth fix, which replaces the EDRAM path's shaders.

## Linux

This is upstream's Linux build, by [@MoonFuji](https://github.com/MoonFuji), tested by upstream on Ubuntu 26.04. **The fork's SDK patches have not been built or run on Linux.** For the tested Linux build, use [upstream's repository](https://github.com/holdmysocks/mcla-recomp) and its README.

To try the fork, you need Clang 20 and LLD (`clang-20`, `clang++-20`, `lld-20`), CMake 3.25+, Ninja, Python 3, Git, a Vulkan 1.3 driver, and these packages (Debian/Ubuntu names): `libgtk-3-dev libx11-xcb-dev libxss-dev libvulkan-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev`. Linux 6.11 or newer is recommended. After the two Linux patches, apply the fork's patches in the order [FORK.md lists them](FORK.md#building-by-hand-or-on-linux), as several change neighbouring lines.

```bash
git clone https://github.com/RenanQueiroz/mcla-recomp.git
cd mcla-recomp
git clone --recursive --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git third_party/rexglue-sdk
cd third_party/rexglue-sdk
git apply ../../patches/rexglue-v0.10.0-mcla.patch
git apply ../../patches/rexglue-minmax-blend-factor.patch
git -C thirdparty/FFmpeg apply ../../../../patches/rexglue-ffmpeg-ps5-config.patch
git apply ../../patches/rexglue-linux-procmap-query.patch
git apply ../../patches/rexglue-linux-wait-sleep.patch
# here the fork's patches, in the order of FORK.md, "Building by hand, or on Linux"
CC=clang-20 CXX=clang++-20 cmake --preset linux-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FSR=ON
cmake --build out/build/linux-amd64 --config Release
cmake --install out/build/linux-amd64 --config Release
cd ../..
python3 scripts/extract_game.py /path/to/your.iso
SDK=$PWD/third_party/rexglue-sdk/out/install/linux-amd64
"$SDK/bin/rexglue" codegen mcla_manifest.toml
CC=clang-20 CXX=clang++-20 cmake --preset linux-amd64-release -DCMAKE_PREFIX_PATH="$SDK"
cmake --build --preset linux-amd64-release
./scripts/play_linux.sh
```

The FSR 3.1 anti-aliasing option (`-DREXGLUE_ENABLE_FSR=ON` above, FSR 3.1 (UNTESTED) in the menu) fetches FidelityFX's source from GitHub when the SDK is configured, and builds its Vulkan library during the SDK's build from the prebuilt shaders that `rexglue-temporal-aa-shaders-vk.patch` adds (FidelityFX's own shader compiler is a Windows program; no Wine is needed), against the SDK's own Vulkan headers and linked to the system's Vulkan loader (`libvulkan.so.1`, there with any Vulkan driver). Without the network CMake warns when configuring and builds the SDK without it; if FidelityFX fails to build, the build warns and goes on without it. Either way `play_linux.sh` says so, and the game offers MSAA and off only. **It has not been built or run on Linux.**

On Wayland, put `vulkan_allow_present_mode_immediate = false` and `vulkan_allow_present_mode_mailbox = false` in `out/build/linux-amd64-release/mcla.toml`; without them the game presents about 1,000 times a second and keeps the GPU busy.

## PS5

![The game installed on a PS5 home screen](docs/images/ps5-home-screen.webp)

*Installed on a jailbroken PS5 with upstream's build. The tile here is one the user supplied at build time; the background is made from the art on their own disc.*

This is upstream's PS5 build. The fork's `ps5/make_ps5.sh` also applies the fork's SDK patches, after upstream's, and the fork's PS5 host asks for a 4,096-tile EDRAM, so the scene is drawn as one tile (single-tile rendering), and it builds FSR 3.1 anti-aliasing in (fetched from GitHub the first time; `MCLA_PS5_FSR=0 bash ps5/make_ps5.sh ...` leaves it out). In the pause menu it is FSR 3.1 (NEXT START, UNTESTED) under PERFORMANCE > ANTI ALIASING: it applies from the next start, as the console's Vulkan device gets the features FSR uses only when the game starts with it chosen, and if it then can't run the setting goes back to MSAA; MSAA stays the default; **none of it has been built or run on a console.** To keep the console's 2,048 tiles, add `edram_tiles = 2048` to `/data/mcla/mcla.toml`, or build with `-DMCLA_EDRAM_TILES=2048`. The shader pack goes to `/data/mcla/shaderpack` too, and the game adds what its storage lacks of it at every start, before the storage loads (a few seconds the first time; UNTESTED; [KNOWN-ISSUES.md](KNOWN-ISSUES.md#shaders-prepared-before-the-first-start-and-kept-prepared-2026-10-17)). For the tested PS5 build, use [upstream's repository](https://github.com/holdmysocks/mcla-recomp): the same steps with `https://github.com/holdmysocks/mcla-recomp.git`.

You need a jailbroken PS5 that runs homebrew, with an FTP server payload (port 2121 by default) and a homebrew mounter that shows folders under `/data/homebrew` on the home screen (ShadowMountPlus was used). The build runs on **Arch Linux as root**: on Windows, an Arch distribution under WSL2. Allow about 30 GB of disk, 16 GB of memory, and about 45 minutes the first time.

On Windows, in PowerShell as administrator:

```powershell
wsl --install --no-distribution
wsl --update
wsl --install -d archlinux
wsl -d archlinux -u root
```

Then, in that Linux shell (your Windows drives are under `/mnt`, so `C:\Games\mcla.iso` is `/mnt/c/Games/mcla.iso`):

```bash
pacman -Sy --noconfirm git
cd /root
git clone https://github.com/RenanQueiroz/mcla-recomp.git
cd mcla-recomp
bash ps5/make_ps5.sh --iso /mnt/c/path/to/your.iso --console 192.168.1.50
```

Use your console's address, and start the FTP payload first. Without `--console` it only builds and prints what to copy where. Keep the repository under `/root`, not `/mnt/c`. The script installs its packages, builds the PS5 toolchain and Vulkan driver ([mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)), patches and builds the SDK, extracts and recompiles your game, packages the title and uploads it. Each step is skipped when its result exists, so you run the same command again after a failure or an update (`git pull` first; if the script then says a patch has changed since it was applied, remove `/root/mcla/rexglue-sdk` so the SDK is patched afresh). Logs are in `/root/mcla/logs`.

After the upload, let the mounter pick up `/data/homebrew/PPSA99779`; "Midnight Club: Los Angeles" appears on the home screen. On the console, `/data/mcla/game` holds the game data, `/data/mcla/mcla.toml` the settings and `/data/mcla/mcla-play.log` the log of the last start.

**Your own tile and theme (optional).** No artwork or music ships here. `--tile IMAGE` takes any common image (for example the [official cover art](https://assets-prd.ignimgs.com/2022/04/17/midnightcomplete-1650239367276.jpg)); `--theme FILE.at9` takes an ATRAC9 file. For a theme, turn the audio down first (`ffmpeg -i in.m4a -af loudnorm=I=-28:TP=-2 -ar 48000 -ac 2 quiet.wav`), then convert it with [AT9-AT3 Converter](https://github.com/BMK-Studio/AT9-AT3_Converter/releases) (AT9, PS4, bitrate 72). The console keeps a title's tile and sound from when it was first registered.

| If | Then |
|---|---|
| `this script needs Arch Linux` | Install Arch under WSL2 as above. |
| `SDK patch does not apply`, another patch does not apply, or a patch has changed since it was applied | Remove `/root/mcla/rexglue-sdk` and run again. |
| `no FTP server at ...` | Start the FTP payload; check the address and `--ftp-port`. |
| A bar with frame rate and temperatures over the game | That is your homebrew enabler's overlay (with OnionHEN, `[overlay] enabled=false` in `/data/OnionHEN/config.ini`). |
| The game stops or closes | Fetch `/data/mcla/mcla-play.log` over FTP before starting it again, and open an issue with it, your console model and firmware. |

If you hand this to an AI agent: it needs your disc image's path and the console's address from you, must not look for or download a disc image, and writes to the console only with `--console`.

## Known issues

[KNOWN-ISSUES.md](KNOWN-ISSUES.md) covers each of the fork's changes: what it fixes, how it was checked and what is not verified. In short:

- Nothing of the fork has been tried on AMD or Intel graphics, on Linux or on the PS5. On Vulkan, the compute texture copies and the direct texture loads are on with NVIDIA only by default (`--vulkan_texture_load_storage_copy=on` tries them elsewhere). D3D12's texture heaps take video memory in 256 MB steps, which matters most on cards with little of it.
- On a first run, in new places, or after a driver update, objects can be missing for a moment while their shaders compile, where the game used to hold the frame until they were ready. With the shader pack (added at every start, compiled by the launcher before Play when needed) that is left only for what the fork's test sessions didn't reach.
- The HUD's gauges and the song title under the speedometer move in the same steps. At *HUD updates* 60 per second (the default) the needles are smooth and the title scrolls twice its original speed; at 30, the console's pace, the title scrolls at its original speed and the needles move 30 times a second; at 120 the title scrolls four times as fast.
- Not verified by hand: the motorcycle lean (it needs a bike in play).
- HDR is new: on by default on Windows whenever Windows has HDR on for the display (signed off on one display, an ASUS ROG Swift PG32UCDP (WOLED, Gaming HDR mode, Uniform Brightness on), 2026-10-16), it outputs HDR10 only (no scRGB), it works on Windows only for now (the PS5 keeps SDR; Linux on Wayland comes later), and the garage, photo mode and cutscenes weren't reached in the tests. Screenshots taken in HDR come out as SDR, with the highlights clipped.

## Missed functions

If the game stops with `Call to invalid or unregistered function at guest address ...`, the recompiler missed a function that is only reached through a pointer. That is a gap in the recompilation, which is upstream's area: please open the issue on [holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp/issues) with that line (the fix is one entry in `config/runtime_discovered.toml`), and the fork takes the fix at its next sync. Problems with the fork's changes or the launcher, or anything that doesn't happen with upstream's build, go to the [fork's issues](https://github.com/RenanQueiroz/mcla-recomp/issues).

## This fork and upstream

The fork takes upstream's changes only when asked to, never automatically: upstream's `main` is merged into the fork's, built and checked, then pushed. The launcher follows the fork's `main` and only reports how many of upstream's commits the fork hasn't taken yet. Each change meant for upstream is one commit here, offered to holdmysocks/mcla-recomp as a pull request from its own branch off upstream's `main`; when upstream merges one, the fork's copy goes away in the next merge. The launcher, the measurement tools and the scripted controller input stay in the fork. The process, the patch order and the list of changes: [FORK.md](FORK.md).

## Credits

- [mcla-recomp](https://github.com/holdmysocks/mcla-recomp) by holdmysocks, the project this fork builds on.
- ReXGlue SDK by Tom Clay and contributors; Xenia by Ben Vanik and contributors.
- [LARecomp](https://github.com/mzzvxm/LARecomp) by mzzvxm and contributors, whose fixes several parts of this project are ported from, with permission. In the fork, the motorcycle lean fix builds on the gyro function LARecomp identified (its pull request #9), and the menu pacing on its notes on the UI manager, used with its author's permission.
- [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) by mihawk-99 (GPL-3): the RADV port and toolchain the PS5 version is built on.
- XenonRecomp by hedge-dev, on which ReXGlue's analysis builds.
- The Linux build by [@MoonFuji](https://github.com/MoonFuji).
- [ForzaHorizon1-ReImagined](https://github.com/Dyotson/ForzaHorizon1-ReImagined) by Dyotson and contributors (MIT): its patches 0036 (Vulkan uploads before the render pass) and 0038 (Vulkan threaded submission), ported to the SDK (MIT notice in the code); the design of the D3D12 threaded submission; and the ideas behind the launcher's `clear_memory_page_state` and `execute_unclipped_draw_vs_on_cpu` defaults.
- [RecompiledHaloReach](https://github.com/Dyotson/RecompiledHaloReach) by Dyotson and contributors (MIT): the achievement toast wrapper in `src/achievement_toast.h`, by way of ForzaHorizon1-ReImagined (MIT notices in the file).
- [xenia-canary](https://github.com/xenia-canary/xenia-canary): the approach of the GPU register writes in bulk; its `VulkanPipelineCache`, after which the Vulkan pipeline cache file is made; its D3D12 pipeline cache, which stopped waiting at the end of a submission the same way; and its shader compile settings, used for the fork's new shaders.
- [Xenia](https://github.com/xenia-project/xenia)'s shader sources at 0b2ffa3, from which the fork's direct texture load and direct resolve shaders, and the fixed 4x MSAA resolve shader, are built.

## Licence

GPL-3.0-or-later; see [`LICENSE`](LICENSE). The parts ported from MIT-licensed projects are listed under Credits, and their licence notices are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). The launcher's icon is the fork's own drawing, made with Windows' Yu Gothic font. *Midnight Club* is a trademark of Take-Two Interactive. This project is not affiliated with or endorsed by Rockstar Games or Take-Two.

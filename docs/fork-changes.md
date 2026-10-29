# The fork's changes in detail

The short version is in the [README](../README.md#what-this-is). How the fork relates to upstream, every change with its effect, the tools that are only in the fork, and the credits in detail.

## How it works

The game's PowerPC code is translated to C++ ahead of time by the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (BSD-3, derived from Xenia), which also provides the Xbox 360 kernel, GPU and audio reimplementation. This repository holds the project manifest, function hints, a small host application, patches to the SDK and the build scripts. The fork adds its own SDK patches to `patches/`, changes to the host application in `src/`, and the launcher in `launcher/`.

## What the fork changes

Each change is kept separate, so that it can be offered upstream as a pull request of its own. The SDK patches and their measurements are in [patches/README.md](../patches/README.md); the game changes, how each was checked and what is not verified, in [KNOWN-ISSUES.md](../KNOWN-ISSUES.md); the list and the order of the patches in [FORK.md](../FORK.md). The figures are from the test machine with the launcher's settings; 3× is 4K.

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
| The game's frame-pacing thread woken on demand: on by itself with 6 or more logical processors (Windows) | the uncapped game is no longer held to 143 to 145 FPS: in the city 146 to 217-240 FPS at 16, 8 and 6 logical processors (+49 to +65%, the 99th percentile frame 23 to 39% shorter); the 120, 60 and 30 FPS caps unchanged; no audio underrun |
| The timer thread waits on a high-resolution timer: on by itself with 5 or fewer logical processors (Windows) | at 4 logical processors in the city: uncapped +17 to 21% FPS, the 99th percentile frame 27-30 ms to 10-12 ms, a 120 FPS cap held (117 to 120), 23 to 58% less CPU, the audio worker's late wake-ups about 300 to about 20 a run |

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
| The XMA audio decoder no longer loses frames in a long stream (the radio plays a song as two mono channels and the SDK's decoder dropped a 512-sample frame in each every 10 to 20 s): Xenia Canary's fixes for it, on by default | the radio's balance and channels stay in step: 0 frames lost in the first 95 s against 6 to 11 (7 against 0 at 4 logical processors), decoded sample for sample like FFmpeg's; fork issue #2 |
| Custom vinyls on cars outside the vinyl editor: the one render target the game bakes them in (512x512, with its mip chain) is read back into guest memory, nothing else is | the arcade race's opponents carry their vinyls (D3D12 and Vulkan, also at 2x resolution; no cost outside the race's loading); the player's own vinyls and the story's cars were not seen |
| The rain drawn at 2x and 3x resolution (the game simulates its drops on the GPU and reads them as vertex data; a scaled resolve did not write them where the vertex shader reads) | the rain is back on D3D12 and Vulkan; no GPU time cost that could be measured, and the four resolves it copies were seen in rain only; fork issue #17 |
| Video memory above native resolution: no host render target taller than 1,024 rows, the ones unused for 60 s freed, and a response when the game nears the OS's budget (the SDK made each target as tall as the console's whole EDRAM and kept it for good) | on an RTX 5080, against v0.2.0 the game's peak video memory went from 4.0 to 2.7 GB at 2x and from 7.5 to 5.0 GB at 3x on D3D12 (Vulkan: 3.9 to 2.7 GB and 7.5 to 4.8 GB), with no frame time cost that we could measure; with a card pretended to have 3.2 GB (D3D12, 2x) free roam ran at 171 to 196 FPS and the pause menu at 155 FPS, where an earlier run with the options off gave 12 and 0.8 FPS, and with 2.6 GB pretended the pressure response kept free roam at 173 to 217 FPS (36 to 50 with it off); not tried on a real 6 GB card or with a pretended card on Vulkan; fork issue #19 |
| Vulkan: a pixel shader's own depth output scaled to the host's float24 depth, as on D3D12 | the car's plate and rear lights are no longer smeared by the wheels' motion blur next to the speedometer (an RTX 5080 and a Radeon iGPU); +0.5% and +0.9% of the frame time, inside the spread between equal runs; fork issue #11 |
| The mouse pointer hidden during play | goes on controller input or after about 3 s still; back when the mouse moves |
| Loading-screen pacing | uncapped free roam at native: mean 81 to 145 FPS; the fast intro speeds up only the start-up logos |
| The pause menu's settings: only what you change is saved, merged into `mcla.toml`, also when the window is closed; the FPS list has 40, 45 and 50; resolution, graphics API and hiding the pointer added; anti-aliasing switches during play; restart-only rows say so, and *(RESTART NEEDED)* once changed; RESTART GAME; the tabs come back after a submenu | command-line options and paths no longer end up in the file, and a file with an old-style path loads again (`patches/rexglue-cvar-toml-strings.patch`); QUIT GAME closes the game (it left it running behind a window that no longer answered); closing waits for a career save being written |
| A display gamma setting: the console's TV ramp (the default) or sRGB for PC monitors, switched during play | on a PC monitor the shadows as the game drew them: a night scene's average brightness 46 to 60 of 255 |
| The launcher edits `mcla.toml` instead of passing options | the launcher and the pause menu show and change the same settings; a choice made in the game sticks |
| Menus, pop-ups and map panels at the console's pace above 30 FPS; the HUD at 60 a second by default, or 30 (the console's pace), 120 or every frame | the pause menu opens in 0.88 s at 120 FPS (0.93 s at 30; 0.18 s before); the song title under the speedometer scrolls at its original speed |
| Alpha to mask compared with a tolerance on its thresholds | no lattice of dots, one pixel in every 2x2 quad, on lamp poles, palm trunks and shelter posts: at the dusk spawn of free roam 61 dots on a pole's rows against 8 with alpha to mask off (D3D12, 1x); the dithered foliage is as it was; fork issue #8 |

Upstream's own fixes, such as the car reflections and the camera-flight warp that never ended, are in the fork too.

## Tools (fork only, never offered upstream)

- `launcher/`: the Windows launcher above ([launcher/README.md](../launcher/README.md)).
- `launcher/dev/`: what the measurements were made with: in-run A/B switching, GPU timestamps and other instrumentation as SDK patches, the analysis scripts, the dropped patches and the dated notes. Nothing in the build uses it.
- `src/dev_pad.cpp`: scripted controller input for unattended runs and benchmarks (`--mcla_pad_script`, `--mcla_pad_pipe` and the others), driven by `launcher/dev/devrun.py` on a copy of the save data. Off unless its options are set ([launcher/README.md](../launcher/README.md#scripted-controller-input-unattended-runs)).

## This fork and upstream

The fork takes upstream's changes only when asked to, never automatically: upstream's `main` is merged into the fork's, built and checked, then pushed. The launcher follows the fork's `main` and only reports how many of upstream's commits the fork hasn't taken yet. Each change meant for upstream is one commit here, offered to holdmysocks/mcla-recomp as a pull request from its own branch off upstream's `main`; when upstream merges one, the fork's copy goes away in the next merge. The launcher, the measurement tools and the scripted controller input stay in the fork. The process, the patch order and the list of changes: [FORK.md](../FORK.md).

## Credits, in detail

The README lists these shortly.

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

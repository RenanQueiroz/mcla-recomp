# MCLA: a fork of mcla-recomp

## What this is

*Midnight Club: Los Angeles Complete Edition* (Xbox 360, title `545407F8`) running natively on a PC. This is a fork of [holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp), the static recompilation of the game to native code for Windows, Linux and jailbroken PS5 consoles, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). On top of upstream's `main` the fork adds performance work on both Windows renderers (Direct3D 12 and Vulkan) so that higher resolutions and frame rates hold, a Windows launcher with a setup wizard that builds, updates and starts the game, HDR, FSR 3.1 and frame generation, and several game fixes. The changes meant for upstream are offered there as pull requests over time.

**This repository contains no game code, assets or keys.** You supply your own legally obtained disc image (`.iso`); the game is built on your PC from it, and the recompiled code is generated locally and never committed. What you build is for you, not for sharing.

## Which release of the game

Only **Midnight Club: Los Angeles Complete Edition, USA/Europe, for Xbox 360** (title id `545407F8`), as a disc image (`.iso`). The fixes in `config/` are addresses in that release's executable; another region or revision would need them checked again.

## Status

| Target | State in this fork |
|---|---|
| Windows, Direct3D 12 | Plays: career, races, garage, audio and a controller. 30 to 120 FPS or uncapped, at native (720p), 2x or 3x (4K). |
| Windows, Vulkan | Runs with the same settings; not yet played through by hand. |
| Linux, Vulkan | Upstream's build plays. The fork's patches have not been built or run there. |
| PS5 (jailbroken) | Upstream's build plays at a steady 30 FPS. The fork's patches have not been built or run on a console. |

Everything the fork adds was built and measured on Windows only, on an NVIDIA RTX 5080 and a Ryzen 7 9800X3D (and on the Ryzen's integrated Radeon). It has not been tried on a discrete AMD card, Intel graphics, Linux or the PS5. If you try it on other hardware, please tell us in the [fork's issues](https://github.com/RenanQueiroz/mcla-recomp/issues), whether it works or not. What was tested and the frame times: [docs/status.md](docs/status.md).

## Windows

You need Windows 10 or 11 (x86-64) with a Direct3D 12 graphics card.

### Install

Download **`MCLA-Setup.exe`** from the [latest release](https://github.com/RenanQueiroz/mcla-recomp/releases/latest) and run it. It installs the launcher for your Windows user (no administrator prompt) and adds Start menu and desktop shortcuts. The launcher then opens a **setup wizard**: pick a folder and your disc image, and it downloads the build tools, builds the game from your disc image and prepares the shaders. It needs the internet and about 10 GB of disk, and the folder's path can be at most 129 characters.

**The exe is not signed** (releases never are), so Windows SmartScreen may stop it the first time with *Windows protected your PC*: click **More info**, then **Run anyway**. If your browser holds the download back, choose **Keep**. Some antivirus programs distrust new unsigned installers; if one removes it, nothing is lost: the source of everything is here, and [docs/building.md](docs/building.md) builds it by hand. More about the wizard, checking the download and the launcher: [docs/install.md](docs/install.md).

### Updating

Open the launcher. When there is a new release it says **An update is available**; press **Update and rebuild**. It downloads what changed and rebuilds only that. Your settings, saves and shader storage are kept.

### The launcher

**Play** starts the game. The launcher's settings are the game's own, kept in `mcla.toml` next to `mcla.exe`, and the Settings tab of the pause menu changes the same ones. After a build or an update the launcher **prepares shaders**, so that places don't stutter while their shaders compile the first time you get there. The **More** menu has rebuild, prepare shaders, the game folder and the build logs; under **Advanced**, *More mcla.exe options* takes any other option (for example `--log_file=run.log`). Everything the launcher does: [launcher/README.md](launcher/README.md).

## Settings

In the launcher, and in the pause menu's Settings tab (resolution and graphics API apply at the next start). Defaults are in bold.

| Setting | Choices | Notes |
|---|---|---|
| Graphics API | **Automatic**, Direct3D 12, Vulkan | Automatic picks Vulkan on Intel and AMD RDNA integrated graphics, Direct3D 12 on everything else. |
| Resolution | **Native (720p)**, 2x (1440p), 3x (4K) | 3x needs by far the most GPU power. |
| Frame rate | **30**, 40, 45, 50, 60, 120 FPS, Uncapped | 40 to 50 suit G-Sync and FreeSync displays. |
| Graphics preset | **Custom**, Low-end, Lowest | Low-end turns off anti-aliasing and the full-screen blur and updates the environment map every 4th frame: about 10% less frame time (estimated, on an integrated Radeon). Lowest also turns the real-time shadows off: about 27% less, and no sun shadows. |
| Anti-aliasing | **MSAA 2x**, FSR 3.1 native AA, Off | Off is 2 to 5% faster at 3x. |
| FSR upscaling | **Off**, Ultra Quality, Quality, Balanced, Performance | Draws the 3D scene smaller and rebuilds it: faster, softer. Needs FSR anti-aliasing. |
| Frame generation | **Off**, FSR 3.1 | A generated frame between each two. For displays of 120 Hz or more, or G-Sync and FreeSync; adds input lag. |
| Anisotropic filtering | Game default, Off, 2x, 4x, 8x, **16x** | Keeps textures sharp at a slant (the road ahead). Costs next to nothing. |
| HDR | Off, **Auto**, On | Auto turns it on whenever Windows has HDR on for the display. Peak brightness, paper white and highlights are in the launcher too. |
| NVIDIA Reflex / AMD Anti-Lag 2 | **Default**, Off, On, On + Boost | Less input lag when the GPU is the limit: on an RTX 5080 at 3x the time to the screen went from 22 to 8 ms, at a cost of 3 to 5% of the frame rate on Direct3D 12. On by default for NVIDIA, off for AMD (untested); always on with frame generation. |

The launcher also has display (fullscreen or windowed), display gamma, screen scaling filter, HUD updates, intro, button prompts, language and player name. Every setting with its `mcla.toml` key: [docs/settings.md](docs/settings.md).

## Turning a change off

Nearly every change the fork makes on by default has an off switch, so a problem can be tested without rebuilding. Type the flag under the launcher's *Advanced* › *More mcla.exe options*, or pass it to `mcla.exe`. If a problem goes away with a change off, please say which flag did it in the [fork's issues](https://github.com/RenanQueiroz/mcla-recomp/issues).

The ones you are most likely to want:

| Change | Off switch |
|---|---|
| Faster pause menu and GPS map | `--mcla_larger_tiles=false` |
| Sun glare follows the real sun | `--occlusion_query_async=false` |
| Less CPU while a frame-rate cap is set | `--mcla_timer_hr_at_cap=false`, `--mcla_reflex_sleep_assist=false` |
| Faster on CPUs with 5 or fewer threads, and audio set up for them | `--mcla_small_cpu_defaults=false` |
| Less video memory at 2x and 3x | `--render_target_max_height=0` |
| Rain at 2x and 3x | `--mcla_rain_unscaled_copy=false` |
| Vinyls on the opponents' cars | `--mcla_vinyl_readback=false` |
| Picking the faster GPU on a PC with two GPUs | `--mcla_prefer_dedicated_gpu=false` |
| The HUD at 60 updates a second | `--mcla_ui_hud_rate=30` |
| 16x anisotropic filtering | the launcher's *Anisotropic filtering* setting |

Every switch (about 40, the graphics and timing internals too), what each does and what was measured: [docs/off-switches.md](docs/off-switches.md).

## If something goes wrong

| If | Then |
|---|---|
| SmartScreen stops `MCLA-Setup.exe` | **More info**, then **Run anyway**. |
| The wizard or the launcher can't reach GitHub | **Copy details** says where it stopped. Try opening the [releases page](https://github.com/RenanQueiroz/mcla-recomp/releases/latest) in a browser, turn a VPN off or on, and check the proxy, the antivirus and the PC's date. The wizard's **Use files I downloaded** works from files you fetched yourself. |
| The game is choppy, or runs at a few frames a second, on a PC with two GPUs (a laptop above all) | It may be on the integrated GPU. Look for the lines starting `GPU:` in the log (below); if the name isn't your graphics card, set `mcla.exe` to *High performance* in Windows' *Settings › System › Display › Graphics* and open an issue with those lines. |
| The game stays at 60 FPS or paces unevenly | Turn VSYNC off in the pause menu's DISPLAY. |
| The launcher says *Shaders weren't prepared* | The game plays as before, compiling shaders as it meets them. The message says why; *More* › *Prepare shaders* tries again. |
| A build step fails | [docs/troubleshooting.md](docs/troubleshooting.md) has the usual ones (a path that is too long, missing tools, an old Visual Studio, PowerShell scripts disabled). |

**Reporting a problem.** Open an issue on the [fork's issues page](https://github.com/RenanQueiroz/mcla-recomp/issues) with what happened, your graphics card and CPU, and the game's log. To get the log, add `--log_file=run.log --log_level=info` under *Advanced* › *More mcla.exe options* and play until it happens; `run.log` is written next to `mcla.exe` (**More** › *Open the game folder*). Its `GPU:` and `CPU:` lines are the most useful. If the game stops with `Call to invalid or unregistered function at guest address ...`, the recompiler missed a function: report that one to [upstream](https://github.com/holdmysocks/mcla-recomp/issues), and anything that doesn't happen with upstream's build to the fork.

## Building from source

On Windows, with Git, CMake, Ninja, Python 3, the .NET 10 SDK (for the launcher) and about 20 GB of disk (the build checks for what it needs and offers to install what is missing):

```powershell
git clone https://github.com/RenanQueiroz/mcla-recomp.git
cd mcla-recomp
.\launcher\build-game.ps1 -Iso C:\path\to\your.iso
.\launcher\build-launcher.ps1
```

`build-game.ps1` clones the ReXGlue SDK, applies the patches, builds it (the long step, once), extracts your game, recompiles it and builds `mcla.exe`; run it again after a failure and it continues. `-Toolchain mingw` builds without Visual Studio or administrator rights. Everything, including building without the launcher: [docs/building.md](docs/building.md).

### Linux

Upstream's build plays; the fork's SDK patches have not been built or run on Linux. Steps: [docs/building.md](docs/building.md#linux).

### PS5

Upstream's build plays on a jailbroken PS5; the fork's patches have not been built or run on a console. Steps: [docs/building.md](docs/building.md#ps5).

## Known issues

- The fork has not been tried on discrete AMD cards, Vega-based Ryzen graphics, Intel graphics, Linux or the PS5.
- On a first run, in new places, or after a driver update, objects can be missing for a moment while their shaders compile. The launcher's shader preparation covers most of it.
- Fine dots may remain at the edges of palm leaves against the sky and in some soft shadows.

Each change, how it was checked and what is not verified: [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

## More documentation

- [docs/settings.md](docs/settings.md): every setting.
- [docs/off-switches.md](docs/off-switches.md): every off switch, with what it does and what was measured.
- [docs/fork-changes.md](docs/fork-changes.md): what the fork changes, and how it relates to upstream.
- [docs/install.md](docs/install.md), [docs/building.md](docs/building.md), [docs/troubleshooting.md](docs/troubleshooting.md), [docs/status.md](docs/status.md).
- [KNOWN-ISSUES.md](KNOWN-ISSUES.md), [launcher/README.md](launcher/README.md), [patches/README.md](patches/README.md) (the SDK patches) and [FORK.md](FORK.md) (the order of the patches, CI, syncing with upstream).
- [Issues](https://github.com/RenanQueiroz/mcla-recomp/issues) for questions and reports.

## Credits

- [mcla-recomp](https://github.com/holdmysocks/mcla-recomp) by holdmysocks, the project this fork builds on; the Linux build is by [@MoonFuji](https://github.com/MoonFuji).
- ReXGlue SDK by Tom Clay and contributors; Xenia by Ben Vanik and contributors; XenonRecomp by hedge-dev, on which ReXGlue's analysis builds.
- [LARecomp](https://github.com/mzzvxm/LARecomp) by mzzvxm and contributors, whose fixes several parts of this project are ported from, with permission.
- [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) by mihawk-99 (GPL-3): the RADV port and toolchain the PS5 version is built on.
- [ForzaHorizon1-ReImagined](https://github.com/Dyotson/ForzaHorizon1-ReImagined) and [RecompiledHaloReach](https://github.com/Dyotson/RecompiledHaloReach) by Dyotson and contributors (MIT), and [xenia-canary](https://github.com/xenia-canary/xenia-canary) and [Xenia](https://github.com/xenia-project/xenia): code and approaches the fork's changes are ported from.

What was taken from each: [docs/fork-changes.md](docs/fork-changes.md#credits-in-detail).

## Licence

GPL-3.0-or-later; see [`LICENSE`](LICENSE). The parts ported from MIT-licensed projects are listed under Credits, and their licence notices are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). The launcher's icon is the fork's own drawing, made with Windows' Yu Gothic font. *Midnight Club* is a trademark of Take-Two Interactive. This project is not affiliated with or endorsed by Rockstar Games or Take-Two.

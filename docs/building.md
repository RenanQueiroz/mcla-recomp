# Building from source

The short version is in the [README](../README.md#building-from-source). Most players don't need this: MCLA-Setup.exe builds the game for you ([install.md](install.md)).

## Windows

You need Windows 10 or 11 (x86-64) with a Direct3D 12 graphics card, and about 20 GB of disk. The build checks for Git, CMake, Ninja, Python 3, Visual Studio Build Tools 2022 version 17.1 or newer (C++ workload), a Windows SDK newer than 10.0.19041 (Windows SDK 10.0.26100 is installed when it is not there) and the project's own Clang 20, and offers to install what is missing (answer `y`, or add `-InstallTools`); the Build Tools ask for administrator rights ([`-Toolchain mingw`](#without-microsofts-compiler-llvm-mingw) needs neither them nor Clang from the LLVM project). It also looks for the Vulkan SDK (`winget install KhronosGroup.VulkanSDK`, administrator rights too), which only the FSR anti-aliasing option needs on Vulkan: without it FSR is built for Direct3D 12 only, and the build goes on. FSR's runtime is built from the FidelityFX SDK's source, which the first build fetches from GitHub; without the network the game builds without FSR. The launcher also needs the .NET 10 SDK (`winget install Microsoft.DotNet.SDK.10`).

### With the launcher (recommended)

In PowerShell:

```powershell
git clone https://github.com/RenanQueiroz/mcla-recomp.git
cd mcla-recomp
.\launcher\build-game.ps1 -Iso C:\path\to\your.iso
.\launcher\build-launcher.ps1
```

`build-game.ps1` runs `scripts\make_pc.ps1`, which clones the ReXGlue SDK v0.10.0 and applies upstream's and the fork's patches, builds it (the long step, once), extracts your game into `game\`, recompiles it and builds `out\build\win-amd64-release\mcla.exe`. Without `-Iso` it takes the first `.iso` in the folder that holds `mcla-recomp`. Each step is skipped when its result is already there, so after a failure you run the same command again. Logs are in `out\logs`. `build-launcher.ps1` builds the launcher into `launcher\app` and puts a **Midnight Club Los Angeles** shortcut on the desktop.

After the build, the launcher's shortcut is the way in; how it works and how it updates are in [install.md](install.md#the-launcher).

### Without the launcher

```powershell
git clone https://github.com/RenanQueiroz/mcla-recomp.git
cd mcla-recomp
.\scripts\make_pc.ps1 -Iso C:\path\to\your.iso
```

This is upstream's one command, with the fork's patches. It leaves a **Midnight Club Los Angeles** shortcut in the repository folder; double-click it, or `mcla.exe` itself. An Xbox or PlayStation controller works.

The options and the pause menu's Settings tab are in [settings.md](settings.md#on-the-command-line-and-in-the-pause-menu).

### Without Microsoft's compiler (llvm-mingw)

`-Toolchain mingw` builds the same game with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20250709 instead of Visual Studio Build Tools: the same Clang (20.1.8) with lld, libc++ and the mingw-w64 headers, on the UCRT that every Windows 10 and 11 has. It needs no Build Tools (several GB and an administrator prompt), and the game it builds needs no Visual C++ runtime (nor do FSR's DLLs, below). `msvc` stays the default.

```powershell
.\launcher\build-game.ps1 -Toolchain mingw -Iso C:\path\to\your.iso   # or scripts\make_pc.ps1 -Toolchain mingw
```

- **The compiler** is downloaded once (a 171 MB zip from llvm-mingw's GitHub release, 661 MB unpacked) into `tools\`, checked against a SHA-256 pinned in the script (a download that does not match is deleted), and used from there. It needs no administrator rights and changes nothing outside the folder: the `PATH` is set for the build's own process only. Git, CMake, Ninja and Python are still needed, and offered through winget as before (CMake's installer may ask for administrator rights); `-ToolsDir C:\tools` uses portable ones instead: it looks in that folder for `llvm-mingw-*\`, `cmake*\` and `ninja*\` (or `ninja.exe`), and anything it does not find there is looked for as usual.
- **Each toolchain keeps its own SDK build** (`third_party\rexglue-sdk\out\build\win-amd64` and `win-amd64-mingw`, with their installs), on one patched copy of the SDK's sources: the first build with the other toolchain builds a second SDK (the long step), and after that changing toolchain rebuilds the game, not the SDK. The game builds into `out\build\win-amd64-release` either way (the launcher and `devrun.py` look there). **What a build was asked for** (`-Toolchain`, `-ToolsDir`, `-SdkBundle` and its SHA-256) is kept in `launcher\build-options.json` when it finishes, however it was run (`build-game.ps1` or `make_pc.ps1`) and when it is more than the defaults; `build-game.ps1` and `make_pc.ps1` given none of them, so **Update and rebuild** too (also an older launcher's, which passes none), build the way the last build did, with the same portable tools or prebuilt SDK, and do not fall back to msvc or to building the SDK from source. Given any of them, only what is given is used; `-Toolchain msvc` switches back. **To switch an install that was built with msvc**, write the record before updating, `Set-Content launcher\build-options.json '{"toolchain": "mingw"}'`: the next update (the launcher's too, whatever version of it) builds with llvm-mingw in one run. Run by the launcher, which can't ask, it fetches the compiler without a question (nothing else is installed unasked), builds the second SDK and the game, and makes FSR's runtime (next point).
- **`mcla.exe` needs `libc++.dll` and `libunwind.dll`** (from llvm-mingw, in place of the Visual C++ runtime, which it then does not need): the build copies them next to it, and next to `rexglue.exe` in the SDK's install. Take them along if you copy the game somewhere else. The line tables of `mcla.exe` (37 MB of its 81) are split off into `mcla.exe.debug` beside it; `llvm-symbolizer` reads them.
- **FSR** (the anti-aliasing option and frame generation) needs FidelityFX's runtime (`amd_fidelityfx_dx12.dll` and `_vk.dll`), which does not build with llvm-mingw (its source includes Microsoft headers). The SDK is built with FSR support in its graphics plugin but without the runtime, which the plugin loads at run time from beside `mcla.exe`; the build says so, and the game offers MSAA and off, unless the runtime is there from elsewhere (the msvc-built DLLs work with a mingw-built game on both APIs): a prebuilt SDK (below), or the same checkout's SDK built once with `-Toolchain msvc`, which this build then takes the DLLs from (and, on a PC with Visual Studio Build Tools and the project's Clang in `tools\`, as an install that used msvc has, builds by itself when they are missing: `make_pc.ps1 -Toolchain msvc -Until sdk`, a minute and a half). **Those DLLs are built with MSVC but have the C runtime linked in** (the SDK builds them with the static runtime: they import only `KERNEL32`, `USER32`, `WINMM`, `dxgi` and `vulkan-1`), so a PC needs no Visual C++ runtime for them. A DLL that is there and doesn't load for another reason is reported unavailable at start (the menu says `RUNTIME NOT LOADED`, and the game goes on with MSAA).
- **`-SdkBundle`** builds the game against a prebuilt SDK instead of cloning, patching and building the SDK, which is most of the first build. It is a folder (used where it is, and trusted as it is: nothing is written into it, and `-SdkBundleSha256` is for a zip), or a `.zip`, a path or an **https** URL (http is refused, and a URL needs `-SdkBundleSha256`: what is downloaded is run, `rexglue.exe` included). A zip is checked against the hash when one is given and unpacked into `third_party\rexglue-sdk-bin`. Either way its layout, the toolchain it was built with, and the SDK commit and the patches it was built from, which its `mcla-sdk-bundle.json` lists (every SDK this script builds has one, and so has a release's bundle), are checked against this checkout's: a bundle made from other patches is refused, saying which, instead of being built against. Bundles are made by the fork's release workflow (`scripts\ci\README.md`). `-Until tools`, `-Until sdk-source` or `-Until sdk` stops after the tools are found or installed, after the SDK's sources are patched, or after the SDK is built (or the bundle checked), before the game's steps: what CI runs, as it has no game.
- **Memory.** Ninja starts a compiler per CPU thread, and one takes up to 0.4 GB. With less than 0.5 GB of free memory per job, the build starts fewer (with either toolchain, and it says so); `CMAKE_BUILD_PARALLEL_LEVEL` sets the number yourself.

A game built this way was compared with the msvc build's on the same Clang version in 62 scripted runs (both APIs, native and 3×): the same pictures, saves, audio and behaviour, and frame times within the benchmarks' own spread (about 2%).

### Updating by hand

See [Updating](install.md#updating).

## Linux

This is upstream's Linux build, by [@MoonFuji](https://github.com/MoonFuji), tested by upstream on Ubuntu 26.04. **The fork's SDK patches have not been built or run on Linux.** For the tested Linux build, use [upstream's repository](https://github.com/holdmysocks/mcla-recomp) and its README.

To try the fork, you need Clang 20 and LLD (`clang-20`, `clang++-20`, `lld-20`), CMake 3.25+, Ninja, Python 3, Git, a Vulkan 1.3 driver, and these packages (Debian/Ubuntu names): `libgtk-3-dev libx11-xcb-dev libxss-dev libvulkan-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev`. Linux 6.11 or newer is recommended. After the two Linux patches, apply the fork's patches in the order [FORK.md lists them](../FORK.md#building-by-hand-or-on-linux), as several change neighbouring lines.

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

The FSR 3.1 anti-aliasing option (`-DREXGLUE_ENABLE_FSR=ON` above, FSR 3.1 (UNTESTED) in the menu) fetches FidelityFX's source from GitHub when the SDK is configured, and builds its Vulkan library during the SDK's build from the prebuilt shaders that `rexglue-temporal-aa-shaders-vk.patch` adds (FidelityFX's own shader compiler is a Windows program; no Wine is needed), against the SDK's own Vulkan headers and linked to the system's Vulkan loader (`libvulkan.so.1`, there with any Vulkan driver). Without the network CMake warns when configuring and builds the SDK without it; if FidelityFX fails to build, the build warns and goes on without it. Either way `play_linux.sh` says so, and the game offers MSAA and off only. The same library has FSR 3.1 frame generation in it (DISPLAY > FRAME GENERATION, FSR 3.1 (UNTESTED) in the pause menu, off by default): the game looks for `libamd_fidelityfx_vk.so` next to the executable and loads it before it offers the row, and says why when it can't (`-DREXGLUE_FSR_FRAMEGEN=OFF` builds the library without frame generation). The Vulkan device is asked for NVIDIA Reflex's extensions (`VK_NV_low_latency2`) on an NVIDIA GPU and `VK_AMD_anti_lag` on an AMD one (both UNTESTED here). **None of it has been built or run on Linux.**

On Wayland, put `vulkan_allow_present_mode_immediate = false` and `vulkan_allow_present_mode_mailbox = false` in `out/build/linux-amd64-release/mcla.toml`; without them the game presents about 1,000 times a second and keeps the GPU busy.

## PS5

![The game installed on a PS5 home screen](images/ps5-home-screen.webp)

*Installed on a jailbroken PS5 with upstream's build. The tile here is one the user supplied at build time; the background is made from the art on their own disc.*

This is upstream's PS5 build. The fork's `ps5/make_ps5.sh` also applies the fork's SDK patches, after upstream's, and the fork's PS5 host asks for a 4,096-tile EDRAM, so the scene is drawn as one tile (single-tile rendering), and it builds FSR 3.1 anti-aliasing in (fetched from GitHub the first time; `MCLA_PS5_FSR=0 bash ps5/make_ps5.sh ...` leaves it out). In the pause menu it is FSR 3.1 (NEXT START) under PERFORMANCE > ANTI ALIASING, and frame generation as FSR 3.1 (NEXT START) under DISPLAY > FRAME GENERATION (built in the same way; `MCLA_PS5_FRAMEGEN=0` leaves it out; its generated and real frames are presented back to back and the console's 59.94 Hz display should space them, so that 30 rendered frames show as 60 (the design: not observed); `frame_generation_pacing = timed` in `/data/mcla/mcla.toml` gives the PC's timed pacing back; the low latency setting is there only if the console's driver offers `VK_AMD_anti_lag`, and is UNTESTED): it applies from the next start, as the console's Vulkan device gets the features FSR uses only when the game starts with it chosen, and if it then can't run the setting goes back to MSAA; MSAA stays the default; **none of it has been built or run on a console.** To keep the console's 2,048 tiles, add `edram_tiles = 2048` to `/data/mcla/mcla.toml`, or build with `-DMCLA_EDRAM_TILES=2048`. The shader pack goes to `/data/mcla/shaderpack` too, and the game adds what its storage lacks of it at every start, before the storage loads (a few seconds the first time; UNTESTED; [KNOWN-ISSUES.md](../KNOWN-ISSUES.md#shaders-prepared-before-the-first-start-and-kept-prepared-2026-10-17-the-pack-grown-and-stale-entries-retired-2026-10-25)). For the tested PS5 build, use [upstream's repository](https://github.com/holdmysocks/mcla-recomp): the same steps with `https://github.com/holdmysocks/mcla-recomp.git`.

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

**DualSense haptics and adaptive triggers (optional; upstream's, by Zaid Naeem).** The game's rumble works on a DualSense out of the box. For haptics from the game's sound and the car (road, gear changes, crashes, nitro) and throttle and brake resistance on R2 and L2, add `mcla_pad_haptics = true` and `mcla_adaptive_triggers = true` to `/data/mcla/mcla.toml`. Tuning options are listed in `ps5/game/ps5_haptics.h` and `ps5/game/ps5_triggers.h`. The fork has not built or run this on a console.

**Your own tile and theme (optional).** No artwork or music ships here. `--tile IMAGE` takes any common image (for example the [official cover art](https://assets-prd.ignimgs.com/2022/04/17/midnightcomplete-1650239367276.jpg)); `--theme FILE.at9` takes an ATRAC9 file. For a theme, turn the audio down first (`ffmpeg -i in.m4a -af loudnorm=I=-28:TP=-2 -ar 48000 -ac 2 quiet.wav`), then convert it with [AT9-AT3 Converter](https://github.com/BMK-Studio/AT9-AT3_Converter/releases) (AT9, PS4, bitrate 72). The console keeps a title's tile and sound from when it was first registered.

| If | Then |
|---|---|
| `this script needs Arch Linux` | Install Arch under WSL2 as above. |
| `SDK patch does not apply`, another patch does not apply, or a patch has changed since it was applied | Remove `/root/mcla/rexglue-sdk` and run again. |
| `no FTP server at ...` | Start the FTP payload; check the address and `--ftp-port`. |
| A bar with frame rate and temperatures over the game | That is your homebrew enabler's overlay (with OnionHEN, `[overlay] enabled=false` in `/data/OnionHEN/config.ini`). |
| The game stops or closes | Fetch `/data/mcla/mcla-play.log` over FTP before starting it again, and open an issue with it, your console model and firmware. |

If you hand this to an AI agent: it needs your disc image's path and the console's address from you, must not look for or download a disc image, and writes to the console only with `--console`.

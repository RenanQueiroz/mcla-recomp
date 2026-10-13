# Building and running the PC version (Linux)

The same game, recompiled natively for x86-64 Linux with the Vulkan backend. Nothing runs through Wine or Proton. As on the other platforms, nothing of the game is in this repository: you build from your own disc image of **Midnight Club: Los Angeles Complete Edition, USA/Europe, Xbox 360** (title id `545407F8`).

State: builds and plays. Free roam holds 30 FPS on an Intel Core Ultra 5 238V laptop (Lunar Lake, integrated Arc 130V, Mesa ANV) with the two Linux patches below and the settings in "Recommended settings". The `play_loop.ps1` recovery loop and the crash tracer are not ported; see "Not ported yet".

Tested on Ubuntu 26.04 (kernel 7.0, glibc 2.43, Mesa 26.1), with the ReXGlue SDK v0.10.0 that the other builds use.

## What you need

| | |
|---|---|
| Clang 20 and LLD | `clang-20`, `clang++-20`, `lld-20`. The SDK's own Linux CI uses the same. |
| CMake 3.25 or newer, Ninja, Python 3, Git | |
| Development packages | The list from the SDK's Linux CI: `libgtk-3-dev libx11-xcb-dev libxss-dev libvulkan-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev` (Debian/Ubuntu names) |
| A Vulkan 1.3 driver | Mesa (ANV, RADV) or NVIDIA's driver |
| Linux 6.11 or newer | Recommended, not required: `rexglue-linux-procmap-query.patch` uses the `PROCMAP_QUERY` ioctl it added, and falls back to the old parse without it |
| Disk | About 20 GB, as on Windows |

## Build

```bash
git clone https://github.com/holdmysocks/mcla-recomp.git
cd mcla-recomp
git clone --recursive --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git third_party/rexglue-sdk
```

If the recursive clone is interrupted, finish it with `git -C third_party/rexglue-sdk submodule update --init --recursive` and check that `git submodule status --recursive` shows no `+` or `-` in front of any entry before going on.

Apply the patches. The first three are the same as on Windows; the last two are the Linux fixes described under "Why the two Linux patches":

```bash
cd third_party/rexglue-sdk
git apply ../../patches/rexglue-v0.10.0-mcla.patch
git apply ../../patches/rexglue-minmax-blend-factor.patch
git -C thirdparty/FFmpeg apply ../../../../patches/rexglue-ffmpeg-ps5-config.patch
git apply ../../patches/rexglue-linux-procmap-query.patch
git apply ../../patches/rexglue-linux-wait-sleep.patch
```

Build and install the SDK (the long step, once):

```bash
CC=clang-20 CXX=clang++-20 cmake --preset linux-amd64 -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF
cmake --build out/build/linux-amd64 --config Release
cmake --install out/build/linux-amd64 --config Release
cd ../..
```

Extract your game, recompile it and build `mcla`:

```bash
python3 scripts/extract_game.py /path/to/your.iso
SDK=$PWD/third_party/rexglue-sdk/out/install/linux-amd64
"$SDK/bin/rexglue" codegen mcla_manifest.toml
CC=clang-20 CXX=clang++-20 cmake --preset linux-amd64-release -DCMAKE_PREFIX_PATH="$SDK"
cmake --build --preset linux-amd64-release
```

The executable is `out/build/linux-amd64-release/mcla`.

## Play

```bash
./scripts/play_linux.sh
```

The SDK loads the Xenos GPU plugin from the executable's folder only, and Linux does not look there for shared libraries either, so the script links `librexruntime.so` and `librexgpu-xenos.so` from the SDK install next to `mcla` on first run and starts it with that folder on the library path. Options go after the script name, as with `mcla.exe` on Windows.

### Recommended settings

Put these in `out/build/linux-amd64-release/mcla.toml`, or pass them as options:

```toml
vulkan_allow_present_mode_immediate = false
vulkan_allow_present_mode_mailbox = false
```

Without them the presenter picks MAILBOX on Wayland and presents as fast as the GPU allows, about 1,000 times a second for a 30 FPS game. On a laptop the GPU then ran at 99.8% and shared its power budget with the CPU. With FIFO, GPU load during free roam fell to 62% and the presenter's thread from 65% of a core to 13%. The first run, uncapped and with MAILBOX, looked like 5 to 10 FPS (not measured); with FIFO and the 30 FPS target, free roam measured 23.6 FPS before the patches.

Shadows off is the setting that helped most after that, as the PERFORMANCE menu says.

## Why the two Linux patches

Both are in the SDK, not in this project, and both have upstream pull requests with fuller versions: rexglue/rexglue-sdk#498 and rexglue/rexglue-sdk#501. The patches here are small versions for v0.10.0.

- **`rexglue-linux-procmap-query.patch`.** On Linux, `memory::QueryProtect` and the old-protection lookup in `memory::Protect` read and parsed all of `/proc/self/maps` on every call, and `MMIOHandler` calls `QueryProtect` on every GPU write-watch fault. A `perf` profile of free roam showed the busiest guest thread spending about 26% of all samples of the process in the kernel generating that file and in `sscanf` and `getline` parsing it, against about 10% in game code. The patch asks the kernel for the one mapping with the `PROCMAP_QUERY` ioctl and keeps the parse as a fallback. Checked against the parse for the start, middle and last byte of every mapping of a test process with split write-watch-style pages, read-only, executable and shared mappings: identical, except the kernel's `[vsyscall]` page, which the ioctl does not report and the SDK never asks about.
- **`rexglue-linux-wait-sleep.patch`.** `PosixConditionBase::WaitMultiple` truncated the remaining time to whole milliseconds before sleeping, so any wait shorter than 1 ms slept 0 and spun. Alertable waits poll in 1 ms slices, so the audio worker, which waits on several handles, never slept: 62% of a core. With the patch it sleeps the real remaining time, at most 1 ms: 1.3%. #501 removes the polling altogether.

Measured on the laptop above, free roam, 30 FPS target, `vulkan_allow_present_mode_*` off, shadows off, LOD and density at 0.5, the game's frame rate logged every 2 seconds:

| Build | Average FPS | Median of each window's slowest frame | Windows at 28 FPS or more | Windows with a frame over 66 ms |
|---|---|---|---|---|
| SDK v0.10.0 with the three existing patches | 23.6 | 63 ms | 14% | 35% |
| Same, built with `-march=native`, game pinned to the performance cores | 23.8 | 60 ms | 26% | 28% |
| Plus `rexglue-linux-procmap-query.patch` | 29.9 | 46 ms | 98% | 9% |

Each row is a separate run of about 100 seconds along different streets, so differences of a few percent are noise.

With both patches and a 60 FPS target, free roam ran at 30 to 47 FPS, about 39 on average, with the GPU 90% busy, so the limit there is the integrated GPU. The rarer spikes of over 100 ms that remain at 30 FPS coincided with new pipelines being created.

Building with `-march=native` and keeping the game on the performance cores with `taskset` made no measurable difference (second row), so neither is part of this guide.

## Not ported yet

- `scripts/play_loop.ps1` and `scripts/make_pc.ps1`: the steps above are the same steps by hand. If the game stops on a function the recompiler missed, add its address to `config/runtime_discovered.toml` and build again, as the loop does.
- The guest crash tracer (`src/crash_trace.cpp`) is Windows only, as `known-issues.md` says.
- The sampling profiler (`--mcla_profile`) is Windows only; `perf record -p $(pgrep -x mcla)` works instead.

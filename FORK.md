# This fork

[RenanQueiroz/mcla-recomp](https://github.com/RenanQueiroz/mcla-recomp) is a fork of
[holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp). It adds two
kinds of changes on top of upstream's `main`:

- **Changes meant for upstream**, one commit each, described in
  `KNOWN-ISSUES.md` (game and results) and `patches/README.md` (SDK patches).
  Each is a candidate for a pull request to
  holdmysocks/mcla-recomp, made from its own branch off upstream's `main`.
  - Game: frame pacing at the swap, the 1 ms timer kept while the window is
    hidden, the achievement toast registered only while it shows, the
    motorcycle lean filter by elapsed time, the mouse pointer hidden during
    play, loading-screen pacing that no longer stutters uncapped, menus at the
    console's pace above 30 FPS with the HUD at a rate of its own, and the pause
    menu's settings: saved as changed, merged into `mcla.toml`, with more rows
    (resolution, graphics API, the pointer, anti-aliasing during play), restart-only
    rows marked, RESTART GAME, and a QUIT GAME that closes the game; closing
    waits for a career save being written; plus the launcher's four SDK
    settings as the game's PC defaults; a display gamma setting, the
    console's TV ramp or sRGB for PC monitors, switched during play; and
    single-tile rendering that takes effect: the main scene with 2x MSAA drawn
    as one tile instead of two, in a 4,096-tile EDRAM the game asks the SDK
    for on PC and on the PS5 (untested on the console), switched during play;
    and HDR output: render markers in the GPU command stream and the DISPLAY
    rows HDR, PEAK BRIGHTNESS, PAPER WHITE and HIGHLIGHTS (`KNOWN-ISSUES.md`).
  - SDK, as patches in `patches/` that `scripts/make_pc.ps1` and
    `ps5/make_ps5.sh` apply (`patches/README.md`): GPU register writes in bulk,
    ImGui dialogs that need no repaints of their own, inline bit helpers,
    batched GPU counters, the shared-memory request fast path (also with hot
    pages), watched pages kept out of the hot set (the attract movie's
    colours), D3D12 command lists submitted on their own thread, a timer queue
    that sleeps instead of spinning, a cursor auto-hide that follows focus
    and controller input, the per-frame counters on Vulkan, Vulkan draws
    that no longer look a setting up in the cvar registry each, Vulkan uploads
    recorded before the render pass instead of ending it, Vulkan command
    buffers recorded and submitted on their own thread, D3D12 textures
    placed in shared heaps instead of each being an allocation of its own (the
    map zoom-out's and races' stutters), and Vulkan texture loads copied into
    their images by a compute shader instead of `vkCmdCopyBufferToImage`
    (NVIDIA only by default; 19% shorter frames at 3x), and the game's
    Direct3D 9-style clear rectangles done as host render target clears
    instead of copying the render targets between MSAA modes and back (13 to
    32% shorter frames at 3x on both APIs), and resolved textures decoded
    straight into the texture instead of through a buffer and a copy
    (4 to 6% shorter frames at 3x on both APIs), and frames no longer held
    while new pipelines compile, with no stand-in pipelines on Vulkan (cold
    sessions: worst frame 1.9 s to 0.25 s on Vulkan, 0.4 s to 0.19 s on
    D3D12), the Vulkan shader storage translated in parallel at start-up,
    and an optional Vulkan pipeline cache file, and resolves that read the
    render target directly instead of dumping it into the EDRAM buffer first,
    at every resolution scale (7 to 12% shorter frames at 3x in the city on
    both APIs, 4 to 9% in a race; at native and 2x the resolve time saved
    matters on GPU-bound systems), with a fix for the 4x MSAA depth samples
    the EDRAM buffer path misread at odd resolution scales, and settings files
    saved as valid TOML, merged into the file and written durably (F4 saving
    only what changed while running), and loaded statement by statement when
    one is bad, and an EDRAM of 4,096 tiles as an option (`edram_tiles`, the
    console's 2,048 by default), for titles patched to draw more at once than
    the console's EDRAM held, and HDR output: the title's markers read from the
    command stream, HDR10 swap chains with the display's levels, the guest's UI
    kept as a separate layer, and this game's scene in HDR from its tone map's
    inputs (`patches/README.md`, `launcher/dev/hdr-2026-10-16.md`), and AMD
    FSR 3.1 as an anti-aliasing option, and the fullscreen setting applied on
    the UI thread (switching it with the F4 or console overlay open could hang
    the game), and UI overlay painting: every guest frame presented by the
    guest output thread while the presenter's own UI is shown, that UI drawn by
    the UI thread into a layer put over the image (`present_ui_overlay`, on by
    default), and FSR 3.1 frame generation (`frame_generation`, off by
    default; the pause menu's DISPLAY › FRAME GENERATION and the launcher's
    Frame generation; it switches on and off during play on both APIs, and has
    a frame-rate cap of its own, `mcla_fps_frame_generation`, Half display by
    default): FidelityFX's frame generation built on Windows; on Direct3D 12 a
    frame generated between every two (HDR or SDR, any anti-aliasing),
    presented through AMD's frame interpolation swap chain; on Vulkan the same,
    generated by the command processor and shown by a frame pacer that copies
    and presents each frame on a queue of its own; offered untested on Linux
    and the PS5; transient settings (values a program works out while running
    are neither saved by the overlay's Save nor read from a config file), and
    the content header file written with zeros in its padding, the same bytes
    from every compiler.
- **Fork-only extras** that have no place upstream: `launcher/`, a Windows
  launcher that checks this fork for updates, builds with `make_pc.ps1`,
  edits the game's settings in `mcla.toml` (shared with the pause menu) and
  starts the game, the measurement tools in
  `launcher/dev/` (`launcher/README.md`), and scripted controller input for
  unattended runs and benchmarks (`src/dev_pad.cpp`, off unless its options
  are set; `launcher/README.md`, "Scripted controller input"), and the
  fork's shader pack (`shaderpack/`, fingerprints and pipeline descriptions
  only): at every start the game rebuilds what its shader storage lacks of it
  from the player's own game files and adds it, under a session lock, with a
  journal that undoes an import whose start didn't finish
  (`src/shader_prepare.cpp`); `mcla.exe --mcla_prepare_shaders` also creates
  every pipeline and quits, which the launcher runs after each build and before
  Play when the game's stamp says the driver, the settings or the storage
  changed; the SDK side is `patches/rexglue-shader-storage-prepare.patch`
  (`KNOWN-ISSUES.md`).

## FSR 3.1 anti-aliasing

`patches/rexglue-temporal-aa.patch` adds AMD FSR 3.1 at native resolution as
an anti-aliasing option (MSAA stays the default). The SDK build fetches the
FidelityFX SDK's source at a pinned, hash-checked commit and builds its
runtime (`amd_fidelityfx_dx12.dll`, and `amd_fidelityfx_vk.dll` with the
Vulkan SDK); without the network or the Vulkan SDK it builds without FSR (or
without FSR on Vulkan), and the game offers MSAA and off only. FidelityFX's
shader compiler is a Windows program, so Linux and the PS5 build the Vulkan
runtime from the prebuilt SPIR-V of the permutations rexglue uses, in a patch
of its own (`patches/rexglue-temporal-aa-shaders-vk.patch`, made by
`launcher/dev/fsr/fsr_vk_shaders.py`); the PS5 links it statically. Neither
has been built or run there yet; frame generation's components are in those
runtimes too (`patches/rexglue-frame-generation-nonwindows.patch`: the Linux
device looks for `libamd_fidelityfx_vk.so`, the PS5 title checks that FidelityFX
is linked, the PS5 presents the generated and the real frame back to back on its
fixed-refresh display; `MCLA_PS5_FRAMEGEN=0` and `REXGLUE_FSR_FRAMEGEN=OFF`
leave them out, and `ps5/make_ps5.sh` builds without them by itself if FidelityFX
fails to build with them, so that FSR anti-aliasing is kept). On
the PS5 FSR is a choice for the next start, marked UNTESTED, as the console's
Vulkan device gets FSR's features only when the game starts with it chosen. A
reactive mask for rain and smoke is in but experimental and off by default.
KNOWN-ISSUES.md has the costs and limits. FidelityFX's runtime does not build
with llvm-mingw (`make_pc.ps1 -Toolchain mingw`): that SDK has FSR support in
its plugin but not the runtime, which comes from a prebuilt SDK or from an
msvc build of the same checkout (the DLLs are plain C to the plugin, and work
with either toolchain's game). They are built with MSVC with the C runtime linked
in, so a PC needs no Visual C++ runtime for them either; a DLL that is there and
doesn't load is reported unavailable and the game goes on with MSAA.

## Testing so far

Everything here was built and measured on **Windows only, with an NVIDIA RTX 5080**
(and a Ryzen 7 9800X3D), on the D3D12 and Vulkan backends. None of it has been
built or run on AMD or Intel graphics, on Linux, or on the PS5 yet. It needs
testing there before anyone relies on it.

## Building and playing

On Windows, the first time: clone this fork, put the disc image (Midnight Club:
Los Angeles Complete Edition, USA/Europe) next to the repository folder, and run
`launcher\build-game.ps1` (or `scripts\make_pc.ps1 -Iso <path>`; see
README.md, "Windows", for the tools). Then `launcher\build-launcher.ps1`
builds the launcher and puts a shortcut to it on the desktop, which updates,
builds and plays from then on.

Two toolchains build it (`-Toolchain`, README.md, "Windows"): `msvc`, the
default, is the project's Clang 20.1.8 with Visual Studio Build Tools' headers
and libraries; `mingw` is llvm-mingw 20250709 (the same Clang version, with lld,
libc++ and the mingw-w64 headers, on Windows' own UCRT), downloaded into
`tools\` from its release page and checked against a pinned SHA-256, with no
Microsoft compiler and no administrator rights. Each keeps its own SDK build
(`third_party\rexglue-sdk\out\build` and `out\install`, `win-amd64` and
`win-amd64-mingw`) and both share the patched SDK sources: the first build
with the other toolchain builds a second SDK, and after that switching
rebuilds the game only. `-ToolsDir` takes portable llvm-mingw, CMake and Ninja
instead of installing them, and `-SdkBundle` a prebuilt SDK (unpacked into
`third_party\rexglue-sdk-bin`, and checked against this checkout's SDK commit
and patches through its `mcla-sdk-bundle.json`) instead of cloning, patching and
building one. `make_pc.ps1` keeps what it was asked for in
`launcher\build-options.json`, and `launcher\build-game.ps1`, given no options,
builds the way the last build did (so does `make_pc.ps1`), so that an update
(also the launcher's, an older one's too) neither falls back to msvc nor to
building the SDK from source; an install built with msvc switches with a
record written before its update (README.md, "Without Microsoft's compiler").

**CI** (`.github/workflows/`, `scripts/ci/README.md`): every push and pull
request checks that the pinned tools and the patch list agree, builds the SDK
as patched with the pinned llvm-mingw (`make_pc.ps1 -Until sdk`), compiles the
host sources in `src/` against it (syntax and types; the recompiled game code,
linking and running need a game), and publishes the launcher. The release
workflow builds the launcher, the SDK bundle (with the FidelityFX DLLs, the C
runtime linked in), the corresponding source of the bundle's GPL and LGPL parts
and the files that describe them (with build-provenance attestations), and
makes a **draft** GitHub release of them; it never publishes one. A patch
added to `patches/` has to be added to `make_pc.ps1` and to the `git apply`
lines below: CI fails if they differ. It also builds the SDK with llvm-mingw,
so a patch that only builds with MSVC's Clang fails there (as
`rexglue-low-latency.patch` did, until its AMD header was changed to build with
both).

## Building by hand, or on Linux

`scripts\make_pc.ps1` and `ps5/make_ps5.sh` apply the fork's SDK patches
themselves. To patch the SDK by hand, first apply the patches upstream's build
applies: on Windows the ones `scripts\make_pc.ps1` applies
(`rexglue-v0.10.0-mcla.patch`, `rexglue-minmax-blend-factor.patch`, and
`rexglue-ffmpeg-ps5-config.patch` in `thirdparty/FFmpeg`); on Linux the
`git apply` lines in README.md, "Linux", which include the two Linux patches.
Then, still in `third_party/rexglue-sdk`, apply the fork's patches in this
order, as several change neighbouring lines (the patch table in
`scripts\make_pc.ps1` is the reference for the order):

```
git apply ../../patches/rexglue-gpu-regwrite-bulk.patch
git apply ../../patches/rexglue-imgui-dialog-repaint-on-demand.patch
git apply ../../patches/rexglue-inline-bit-helpers.patch
git apply ../../patches/rexglue-perf-counters-batched.patch
git apply ../../patches/rexglue-shared-memory-request-fast-path.patch
git apply ../../patches/rexglue-threaded-submission-d3d12.patch
git apply ../../patches/rexglue-timer-queue-blocking-wait.patch
git apply ../../patches/rexglue-shared-memory-fast-path-hot-pages.patch
git apply ../../patches/rexglue-hot-pages-watched-ranges.patch
git apply ../../patches/rexglue-cursor-auto-hide.patch
git apply ../../patches/rexglue-vulkan-memexport-check-order.patch
git apply ../../patches/rexglue-vulkan-hoist-shared-memory-uploads.patch
git apply ../../patches/rexglue-threaded-submission-vulkan.patch
git apply ../../patches/rexglue-d3d12-placed-texture-heaps.patch
git apply ../../patches/rexglue-vulkan-texture-load-storage-copy.patch
git apply ../../patches/rexglue-clear-rectangle-draws.patch
git apply ../../patches/rexglue-texture-load-direct.patch
git apply ../../patches/rexglue-async-pipelines-no-frame-wait.patch
git apply ../../patches/rexglue-vulkan-pipeline-storage-parallel-translation.patch
git apply ../../patches/rexglue-vulkan-pipeline-cache-file.patch
git apply ../../patches/rexglue-resolve-4xmsaa-depth-samples.patch
git apply ../../patches/rexglue-direct-resolve.patch
git apply ../../patches/rexglue-cvar-toml-strings.patch
git apply ../../patches/rexglue-edram-size.patch
git apply ../../patches/rexglue-guest-markers.patch
git apply ../../patches/rexglue-hdr-output.patch
git apply ../../patches/rexglue-hdr-ui-layer.patch
git apply ../../patches/rexglue-hdr-scene-mcla.patch
git apply ../../patches/rexglue-temporal-aa.patch
git apply ../../patches/rexglue-temporal-aa-shaders-vk.patch
git apply ../../patches/rexglue-fullscreen-setting-ui-thread.patch
git apply ../../patches/rexglue-presenter-ui-overlay.patch
git apply ../../patches/rexglue-frame-generation.patch
git apply ../../patches/rexglue-vulkan-paint-pipeline-format.patch
git apply ../../patches/rexglue-vulkan-present-semaphore-per-image.patch
git apply ../../patches/rexglue-d3d12-storage-preload-threads.patch
git apply ../../patches/rexglue-vulkan-pipeline-storage-keep-unsupported.patch
git apply ../../patches/rexglue-shader-storage-prepare.patch
git apply ../../patches/rexglue-content-header-padding.patch
git apply ../../patches/rexglue-low-latency.patch
git apply ../../patches/rexglue-frame-generation-nonwindows.patch
git apply ../../patches/rexglue-upstream-storage-flush.patch
git apply ../../patches/rexglue-upstream-host-fp-state.patch
git apply ../../patches/rexglue-upstream-linux-wine.patch
git apply ../../patches/rexglue-upstream-small-fixes.patch
git apply ../../patches/rexglue-mingw.patch
git -C thirdparty/FFmpeg apply ../../../../patches/rexglue-ffmpeg-mingw-config.patch
```

The last two are for building on Windows with llvm-mingw (`scripts\make_pc.ps1
-Toolchain mingw`, below); `make_pc.ps1` applies them with either toolchain,
since each change in them is guarded by the compiler or has no effect for any
other. On Linux and the PS5 they are not needed (they apply, and change nothing
there).

The fork's SDK patches are built and tested on Windows only. On Linux they
apply in this order on top of the two Linux patches, but have not been built
or run there.

The two scripts record each patch they apply, with its SHA-256, in the SDK
checkout's `.mcla-patches`, and stop at a patch that is in without its
current hash there: one revised since, or an SDK patched by hand or by an
older script. Patch the SDK afresh then (`launcher\build-game.ps1 -ResetSdk`,
or delete the SDK checkout and run the script again); `patches/README.md` has
the details.

## Keeping up with upstream

The fork takes upstream's changes **only when asked to**, never automatically:
fetch `upstream`, merge `upstream/main` into `main`, build and check, then push.
The launcher follows this fork's `main` and only reports how many upstream
commits it has not taken yet. When upstream merges one of the changes above,
the fork's copy of it goes away in that merge.

```
git remote add upstream https://github.com/holdmysocks/mcla-recomp.git
git fetch upstream
git merge upstream/main
```

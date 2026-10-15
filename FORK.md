# This fork

[RenanQueiroz/mcla-recomp](https://github.com/RenanQueiroz/mcla-recomp) is a fork of
[holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp). It adds two
kinds of changes on top of upstream's `main`:

- **Changes meant for upstream**, one commit each, described where upstream
  describes such things. Each is a candidate for a pull request to
  holdmysocks/mcla-recomp, made from its own branch off upstream's `main`.
  - Game: frame pacing at the swap, the 1 ms timer kept while the window is
    hidden, the achievement toast registered only while it shows, the
    motorcycle lean filter by elapsed time, the mouse pointer hidden during
    play, loading-screen pacing that no longer stutters uncapped, and menus (and,
    optionally, the HUD's animations) at the console's pace above 30 FPS (`docs/known-issues.md`,
    2026-10-14 sections).
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
    and an optional Vulkan pipeline cache file.
- **Fork-only extras** that have no place upstream: `launcher/`, a Windows
  launcher that checks this fork for updates, builds with `make_pc.ps1` and
  starts the game with chosen options, the measurement tools in
  `launcher/dev/` (`launcher/README.md`), and scripted controller input for
  unattended runs and benchmarks (`src/dev_pad.cpp`, off unless its options
  are set; `launcher/README.md`, "Scripted controller input").

## Testing so far

Everything here was built and measured on **Windows only, with an NVIDIA RTX 5080**
(and a Ryzen 7 9800X3D), on the D3D12 and Vulkan backends. None of it has been
built or run on AMD or Intel graphics, on Linux, or on the PS5 yet. It needs
testing there before anyone relies on it.

## Building and playing

On Windows, the first time: clone this fork, put the disc image (Midnight Club:
Los Angeles Complete Edition, USA/Europe) next to the repository folder, and run
`launcher\build-game.ps1` (or `scripts\make_pc.ps1 -Iso <path>`; see
`docs/pc-build-guide.md` for the tools). Then `launcher\build-launcher.ps1`
builds the launcher and puts a shortcut to it on the desktop, which updates,
builds and plays from then on.

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

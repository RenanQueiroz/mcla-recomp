# Shader preparation (2026-10-17)

Windows 11, RTX 5080, Ryzen 7 9800X3D, 3x (4K), windowed, the launcher's settings.

## What it is

`mcla.exe --mcla_prepare_shaders=true` (`src/shader_prepare.cpp`), which the launcher runs after each build and from *More* › *Prepare shaders*:

1. reads the shader pack, `shaderpack/545407F8.shaderpack`, and checks it: its own hash, the storage versions of this build's SDK (`include/rex/graphics/shader_storage.h`, from `rexglue-shader-storage-prepare.patch`), and fingerprints of two places in the player's decrypted `default.xex` (the archive key and the vertex type table);
2. reads the effects (`shaders/*/fxl_final`) and the vertex presets (`.sps`) from `xarchive_cache.rpf`, with the key found in guest memory by its fingerprint (an AES-256 decryptor of its own, and a raw deflate decoder after Mark Adler's puff.c, `THIRD-PARTY-NOTICES.md`);
3. rebuilds every shader of the pack:
   - pixel shaders: an effect's program, by fingerprint, as it is;
   - three shaders of the console's Direct3D: a range of `default.xex`;
   - vertex shaders: the effect's program bound by the game's own recompiled Direct3D code (the constructor `sub_82417B78` and `IDirect3DVertexShader9::Bind`, `sub_82424EB0`) to the pixel shaders the pack names and to vertex layouts generated from the presets and the vertex type table, in guest memory of its own (not the game's heap), until the XXH3 of the result is the pack's;
4. adds what it rebuilt to the storage (`<cache>/shaders/shareable`): the `.xsh`, and the pipeline descriptions whose shaders it has. Entries already in a file of this build's versions stay, in their order; a file of other versions is replaced, as the SDK would empty it at the next start anyway; each file is written beside the old one, flushed and renamed over it; shaders already stored aren't rebuilt;
5. lets the SDK start as usual, which creates every stored pipeline of the API in use, blocking (`shader_storage_progress`, from the same patch, prints the progress);
6. quits before the game's code starts, with the window hidden. It never writes `mcla.toml`; if anything doesn't match it prints why and exits with a code (2 no pack, 3 damaged pack, 4 other game files, 5 other storage versions, 6 can't write), before writing anything.

The pack holds no game data: per shader its fingerprint, its kind, its source (a program's fingerprint, or an address and length in `default.xex`) and the pixel shaders it was linked with; the pipeline descriptions as the storage holds them (render state and shader fingerprints); the versions and the two fingerprints. `launcher/dev/shaderpack/build_pack.py` builds it from shader storages and refuses anything else: it re-reads the pack, checks every field, every program fingerprint, every record's hash, and that no 8 bytes of the pack appear in the game's shader microcode. It then runs `mcla.exe` on the pack in a check mode (`--mcla_prepare_shaders_check=<report>`, through `devrun.py`, writing nothing), and accepts the pack only if every shader is rebuilt.

## The pack

- Sources: copies of the agents' development storages and, with the user's agreement, read-only copies of the user's own (its files' SHA-256 and times compared before and after each copy).
- 723 shaders (every shader of those storages has a source in the game's files), pipelines: 2,928 D3D12 (`rtv`), 570 D3D12 rasterizer-ordered views (`rov`), 1,561 Vulkan (`fbo`), 474 Vulkan fragment shader interlock (`fsi`); 411,416 bytes.
- The check: `mcla.exe` rebuilt 723 of 723.

## Method

- **Cold driver cache:** the dev patch `launcher/dev/shader-salt.sdk.patch` (the salt part of `shader-stutter-timers.sdk.patch`) adds `tmp_shader_salt`, which makes the translated shaders new to the driver's cache without changing what they do. A new salt per measurement stands for a first run, or the first after a driver update.
- **Runs:** `launcher/dev/shaderpack/measure.py`, through `devrun.py` (private data and storage folders, the real save checked before and after; the game's standard output to a file, as the launcher reads it).
  - `prepare`: a preparation on a new, empty storage; `--twice` a second one on it (the driver's cache then warm).
  - `race`: `race.pad` under `--mcla_fixed_dt=60`, uncapped (the title, the map zoom-out, the menus into the arcade race, its intro and 90 s of it), on a new storage; with `--prepared` the storage is prepared first with the same salt, as the launcher's step leaves it. Frames counted from the title on. "Compiled in play" is what the storage holds after the run minus what the start created from it: the pipelines compiled while playing, each a draw left out (objects missing) while it compiled.

## Results

### The preparation

From an empty storage unless noted; "rebuild" includes reading the pack and the game's files, "pipelines" is the SDK's creation of the stored pipelines of the API in use; memory at the end, with every pipeline created (process private bytes, and the adapter's local video memory in use).

| API | Driver's cache | Rebuild | Pipelines | Whole run | Private memory | Video memory | Salt |
|---|---|---|---|---|---|---|---|
| D3D12 | cold | 3.43, 3.40, 3.66 s | 2,928 in 1.16, 1.20, 1.29 s | 8 s | 1,068, 1,079, 1,076 MB | 238 MB | 7002, 7005, 7011 |
| D3D12 | warm | 3.92 s | 2,928 in 0.09 s | | 1,018 MB | 238 MB | 7002 again |
| D3D12 | warm, storage already prepared | 0.04 s (723 already stored) | 2,928 in 0.08 s | 4 s | 1,015 MB | 238 MB | 7002 |
| D3D12 | pack without pipelines (baseline) | 3.55 s | 0 | | 767 MB | 238 MB | 7008 |
| Vulkan | cold | 3.39, 3.57, 3.53 s | 1,561 in 8.49, 8.56, 8.46 s | 16 s | 1,413, 1,427, 1,427 MB | 328 MB | 7003, 7007, 7013 |
| Vulkan | warm | 3.55 s | 1,561 in 0.17 s | | 1,239 MB | 328 MB | 7003 again |
| Vulkan | warm, storage already prepared | 0.04 s | 1,561 in 0.12 s | 4 s | 1,240 MB | 328 MB | 7003 |
| Vulkan | pack without pipelines (baseline) | 3.58 s | 0 | | 864 MB | 287 MB | 7008 |

- Each preparation added 723 shaders and 5,533 pipeline descriptions to an empty storage (all four lists, so a later change of API finds its pipelines stored too), and nothing to a storage it had prepared.
- With every pipeline resident: D3D12 about 250 MB more process memory than without them (300 MB right after a cold compile) and no more video memory; Vulkan about 375 MB more (550 MB right after a cold compile) and 41 MB more video memory.
- The rebuild runs on one thread; on a slower CPU it takes longer in proportion. The pipelines are created on the SDK's creation threads, as at any start.

### The race benchmark

Two pairs per API, each run on a new storage with a new salt. "Prepared": the storage prepared with the same salt first, as the launcher leaves it.

| API | Storage | Compiled in play | Frames >50 ms | >100 ms | Worst | Time in frames >50 ms | Zoom-out 1% low | Race 1% low | Run |
|---|---|---|---|---|---|---|---|---|---|
| D3D12 | empty | 955 | 2 | 1 | 113 ms | 0.17 s | 24.3 FPS | 78.8 FPS | 20261017-174550 |
| D3D12 | empty | 975 | 2 | 1 | 114 ms | 0.17 s | 25.9 FPS | 78.2 FPS | 20261017-181116 |
| D3D12 | prepared | 0 | 2 | 1 | 125 ms | 0.18 s | 29.6 FPS | 85.8 FPS | 20261017-174724 |
| D3D12 | prepared | 0 | 2 | 1 | 130 ms | 0.18 s | 28.9 FPS | 83.7 FPS | 20261017-181252 |
| Vulkan | empty | 743 | 4 | 2 | 135 ms | 0.37 s | 14.3 FPS | 61.5 FPS | 20261017-174939 |
| Vulkan | empty | 759 | 1 | 1 | 130 ms | 0.13 s | 23.5 FPS | 65.1 FPS | 20261017-181426 |
| Vulkan | prepared | 0 | 2 | 1 | 144 ms | 0.19 s | 31.4 FPS | 73.6 FPS | 20261017-175130 |
| Vulkan | prepared | 0 | 4 | 1 | 137 ms | 0.34 s | 28.6 FPS | 74.3 FPS | 20261017-181617 |

- **Compiled in play:** every pipeline the route needs is in the pack, so the prepared runs created none while playing: no draw was left out waiting for one.
- **The slow frames:** in every run the worst frame (113 to 144 ms) is in the menus, while the race loads; with the storage empty, the map zoom-out also had frames of 56 and 133 ms, none prepared. The count of frames over 50 ms barely changes: since 2026-10-15 a frame no longer waits for its pipelines (`shader-stutter-2026-10-15.md`), so what preparation removes is mostly the draws left out, and the slower frames around them (the 1% lows).
- **The next start:** the prepared runs' starts created 2,928 (D3D12, in 63 ms) and 1,561 (Vulkan, in 0.27 s) pipelines from the storage before the logos.

### Refusals, merging, settings

With a copy of an agent's storage (504 shaders, 738 D3D12 pipelines) as the storage, through `devrun.py`:

| Pack | Exit code | Message | Storage |
|---|---|---|---|
| a byte changed | 3 | the shader pack is damaged | unchanged, byte for byte |
| cut in half | 3 | the shader pack is damaged | unchanged |
| another `default.xex` fingerprint | 4 | this default.xex is not the one the shader pack was made for | unchanged |
| other storage versions | 5 | the shader pack was made for another version of the graphics code | unchanged |
| no pack | 2 | no shader pack at ... | unchanged |
| the pack | 0 | merge: 219 shaders and 4,795 pipelines added | every file's old content is the start of its new content |

The last run started with an `mcla.toml` (`fullscreen = true`, `mcla_fps = 60`, a comment), which was the same byte for byte afterwards. The real save data was checked before and after every run, and the real shader storage was only ever copied (its SHA-256 and times compared before and after).

### Frame generation

The frame generation gate (`fg/present-gate.pad`, D3D12, native, 120 FPS cap, `--frame_generation=fsr --mcla_anti_aliasing=fsr`, HDR) on the build with these changes: frame generation started through AMD's swap chain, and in the 40 s steady drive every one of the 4,801 guest frames was presented once (none twice, none dropped). PresentMon needs an elevated prompt here to capture, so the shown-frame rate (2.0 presents per swap on 2026-10-17) wasn't measured again; nothing in these changes reaches the presenter.

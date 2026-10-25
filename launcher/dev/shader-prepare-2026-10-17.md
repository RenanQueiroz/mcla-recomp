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
- (The pack of 2026-10-17; the grown pack of 2026-10-25 is at the end: 762 shaders, 5,876 descriptions, 436,640 bytes.) 723 shaders (every shader of those storages has a source in the game's files), pipelines: 2,928 D3D12 (`rtv`), 570 D3D12 rasterizer-ordered views (`rov`), 1,561 Vulkan (`fbo`), 474 Vulkan fragment shader interlock (`fsi`); 411,416 bytes.
- The check: `mcla.exe` rebuilt 723 of 723.

## Method

- **Cold driver cache:** the dev patch `launcher/dev/shader-salt.sdk.patch` (the salt part of `shader-stutter-timers.sdk.patch`) adds `tmp_shader_salt`, which makes the translated shaders new to the driver's cache without changing what they do. A new salt per measurement stands for a first run, or the first after a driver update.
- **Runs:** `launcher/dev/shaderpack/measure.py`, through `devrun.py` (private data and storage folders, the real save checked before and after; the game's standard output to a file, as the launcher reads it).
  - `prepare`: a preparation on a new, empty storage; `--twice` a second one on it (the driver's cache then warm).
  - `race`: `race.pad` under `--mcla_fixed_dt=60`, uncapped (the title, the map zoom-out, the menus into the arcade race, its intro and 90 s of it), on a new storage; with `--prepared` the storage is prepared first with the same salt, as the launcher's step leaves it. Frames counted from the title on. "Compiled in play" is what the storage holds after the run minus what the start created from it: the pipelines compiled while playing, each a draw left out (objects missing) while it compiled.

## Results

### The preparation

From an empty storage unless noted; "rebuild" includes reading the pack and the game's files, "pipelines" is the SDK's creation of the stored pipelines of the API in use; memory at the end, with every pipeline created (process private bytes, and the adapter's local video memory in use).

| API | Driver's cache | Rebuild | Pipelines | devrun's run (see below) | Private memory | Video memory | Salt |
|---|---|---|---|---|---|---|---|
| D3D12 | cold | 3.43, 3.40, 3.66 s | 2,928 in 1.16, 1.20, 1.29 s | 8 s | 1,068, 1,079, 1,076 MB | 238 MB | 7002, 7005, 7011 |
| D3D12 | warm | 3.92 s | 2,928 in 0.09 s | | 1,018 MB | 238 MB | 7002 again |
| D3D12 | warm, storage already prepared | 0.04 s (723 already stored) | 2,928 in 0.08 s | 4 s | 1,015 MB | 238 MB | 7002 |
| D3D12 | pack without pipelines (baseline) | 3.55 s | 0 | | 767 MB | 238 MB | 7008 |
| Vulkan | cold | 3.39, 3.57, 3.53 s | 1,561 in 8.49, 8.56, 8.46 s | 16 s | 1,413, 1,427, 1,427 MB | 328 MB | 7003, 7007, 7013 |
| Vulkan | warm | 3.55 s | 1,561 in 0.17 s | | 1,239 MB | 328 MB | 7003 again |
| Vulkan | warm, storage already prepared | 0.04 s | 1,561 in 0.12 s | 4 s | 1,240 MB | 328 MB | 7003 |
| Vulkan | pack without pipelines (baseline) | 3.58 s | 0 | | 864 MB | 287 MB | 7008 |

- The "devrun's run" column is the measuring script's own time for the whole run, rounded to the second, its waiting before and after the game included: not the preparation's. The preparation's own times, from the process's creation to its report, were measured later the same day (below): D3D12 7.5 s and Vulkan 15.2 s cold, 2.7 and 2.8 s again on a prepared storage.
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

## Later the same day: the review's fixes, and the import at every start

The review of the first version (no high findings, four medium, eight low) and the user's request for automatic re-preparation changed it:

- **The import at every start** (`mcla_shader_pack_auto`, on): before the storage loads, every start adds what the storage lacks of the pack. Nothing missing: the pack and the storage files are read and checked, 1 ms here. Everything missing (an empty or deleted storage): 3.5 s here for the 723 shaders (one thread), 1.5 s for 422. It replaces the PS5's opt-in preparation at every start, and covers starts without the launcher.
- **M1:** files are read with `CreateFileW` (POSIX `open`): a file that exists but can't be read throws, "in use" (exit 7) on a sharing or lock violation, an I/O error (6) otherwise, before anything is written. Only "not found" counts as missing.
- **M2:** a fixed table of the four lists, with headers built from the SDK's constants and their record sizes; a pack list of another name, header, record size, or given twice, is refused (3). File names never come from the pack.
- **M3:** a session lock (`shaders/local/545407F8.lock`, `LockFileEx` on a byte past the owner text, `flock` on POSIX), taken by every `mcla.exe` before it reads the storage and held until it ends, with its owner ("game <pid>", "prepare <pid>") in the file. A preparation that can't take it exits 7 at once; a start waits up to 2 minutes while a preparation holds it, and doesn't wait (or import) beside another game. Temporary files are `<file>.<pid>.tmp`, and ones left by a process that ended mid-write are removed under the lock.
- **M4:** a journal (`shaders/local/545407F8.import`) with each file's valid length before the import, written before the first rename; phase `rebuild` before the shaders are rebuilt, `merged` before the files are replaced, `done` once the stored pipelines were all created. A start that finds `merged` cuts the files back (a file the import created or replaced is removed); one that finds `rebuild` counts it too. Two such starts in a row with the same pack disable its import (the player's own entries are used); `--mcla_prepare_shaders_retry` (More > Prepare shaders) or deleting the journal tries again. A failure while replacing files puts back the ones already replaced at once.
- **L1:** `CreateFileW`, `WriteFile`, a checked `FlushFileBuffers`, then `MoveFileExW` with retries for 2 s on sharing or access errors; the temporary file removed on any failure; `fsync` of the file and its folder on POSIX.
- **L2:** the archive walk visits each entry once, takes only child ranges inside the table and after their folder, caps sizes by the archive's and decompression by the stated size, and needs at least one entry; the program scan and the range checks are in 64 bits; a vertex shader's fetch count is at most 32 and its fetch table must lie inside the header (in the game's own programs it can end exactly at the data, so the review's extra 31 words don't hold), and the binder's area check adds the 16-byte alignment.
- **L3:** a preparation run sets `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)`: a crash ends it at once. A crash in the game's code while rebuilding counts as a failed start (the journal's `rebuild` phase).
- **L4:** Play returns while a preparation runs, and starting one cancels the countdown; the hidden `mcla.exe` is in a job object that kills it with the launcher; log writes catch `IOException`, a faulted run reports -1; the progress lines are split fully (`preload-done` shows its count); code 3 shows the game's message; the run's game log is `logs/prepare-shaders-game.log`, not the user's `--log_file`; closing the launcher while a build asks for confirmation no longer stops a preparation.
- **L5:** `mcla_prepare_shaders` is taken only from the command line (a line in `mcla.toml` is logged and ignored).
- **L6:** `fullscreen = false` and `shader_storage_progress = 50` are set as command-line values, which `mcla.toml` never saves.
- **L7:** the preparation ends with `ShaderStorageReport(shutdown)`, which closes the storage first: with `vulkan_pipeline_cache_file`, `shaders/local/545407F8.vk.bin` (27 MB here) is written by the preparation.
- **L8:** a program's own type must match the pack's pixel flag; an empty cache folder (or `store_shaders` off) refuses; the stray `preload 0 N` of D3D12's non-blocking preload is gone.
- **Exit codes:** 7 is "in use, try again later" (the launcher tries again at the next Play); 2 to 6 are permanent (not tried again before every Play for the same pack).

**The stamp and the check before Play.** After every start and every preparation the game writes `shaders/local/545407F8.stamp.json`: the pack, the storage versions, the SDK's report (the API, the pipeline storage file, its descriptions and the pipelines created, the adapter with its driver: D3D12's user-mode driver version, Vulkan's `driverVersion` and `pipelineCacheUUID`, the resolution scale, the EDRAM size), the settings, each file's size (and record count), and the display adapters with their drivers as DXGI reports them. At Play, `ShaderStamp.cs` compares it with the pack in the checkout, the API and resolution scale of the next start (the launcher's options, `REX_` environment variables, `mcla.toml`; "any" takes whichever API was used), the display drivers now and the files now. Checked with `launcher-check` (the launcher's code without its window) on a stamp from a real start, each case on its own:

| Case | Reason | Time (new process / again) |
|---|---|---|
| unchanged | none | 25 / 0 ms |
| `--resolution_scale=2` | the resolution changed | 12 / 0 ms |
| `--mcla_gpu_backend=vulkan` | the graphics API changed | 12 / 0 ms |
| `any` | none | 23 / 0 ms |
| no resolution set (1) | none | 23 / 0 ms |
| the recorded driver 31.0.15.1111 | the graphics driver was updated | 24 / 0 ms |
| the recorded device another | the graphics card changed | 22 / 0 ms |
| the recorded pack another | the shader pack changed | 11 / 0 ms |
| the same, the pack skipped after a failure | none | 1 / 0 ms |
| the D3D12 list cut to 100,000 bytes | the shader cache was reset | 12 / 0 ms |
| the D3D12 list deleted | the shader cache was deleted | 12 / 0 ms |
| the journal at `merged` | the last start ended while shaders were being added | 8 / 0 ms |
| no stamp | shaders haven't been prepared yet | 8 / 0 ms |
| a damaged stamp | the shader storage's record can't be read | 12 / 0 ms |
| `mcla.toml` with `resolution_scale = 3`, `mcla_gpu_backend = "vulkan"` | the graphics API changed | |

The preparation's own handling, on a stand-in game (`launcher-check`'s stand-in mode): the progress lines, exit 0 with the count of pipelines, exit 3 and 7 with their messages, `import-disabled`, the user's `--log_file` replaced by the run's own, and a launcher process killed mid-run taking the stand-in with it (the job object).

**Tests** (`launcher/dev/shaderpack/test_storage.py`, through `devrun.py`, on the clean build): all 22 pass.
- M1: a preparation with the shader file held exits 7, nothing written, no journal; a start with a list held starts, without an import.
- M2: lists named `xsh` or `/../../../a.b`, a record size of 68 for `rtv`, another header, a list given twice: exit 3, nothing written anywhere.
- M3: a preparation beside a game's lock exits 7 within its 2 s; a start beside another game neither waits nor imports; a start during a preparation's lock waits for it (12 s), then imports.
- M4: a crash right after the import (`--mcla_shader_pack_test_crash=1`, exit 99) leaves `phase merged`; the next start cuts 5 files back and (crashing again) records one failure; the next disables the pack, the storage back to the player's byte for byte; a normal start keeps it disabled; the retry imports again (422 shaders, 4,533 pipelines) and resets the count. A crash while rebuilding (`=2`) counts too.
- At a start: an empty storage filled with the pack and the stamp written; nothing missing in 1 ms; files cut mid-record completed; a shader file of another version replaced.

**Re-measured** (3x, `tmp_shader_salt` 8001 and 8002, the preparation's own times from the process's creation to its report):

| API | Storage, driver's cache | Rebuild | Pipelines | Process | Private memory | Video memory |
|---|---|---|---|---|---|---|
| D3D12 | empty, cold | 3.54 s | 2,928 in 1.29 s | 7.53 s | 1,074 MB | 238 MB |
| D3D12 | prepared, warm | 1 ms (nothing missing) | 2,928 in 0.08 s | 2.69 s | 1,020 MB | 238 MB |
| D3D12 | empty, warm | 3.55 s | 2,928 in 0.07 s | 6.28 s | 1,019 MB | 238 MB |
| D3D12 | pack without pipelines | 3.59 s | 0 | 6.26 s | 767 MB | 238 MB |
| Vulkan | empty, cold | 3.54 s | 1,561 in 8.92 s | 15.18 s | 1,398 MB | 328 MB |
| Vulkan | prepared, warm | 1 ms | 1,561 in 0.12 s | 2.80 s | 1,236 MB | 328 MB |
| Vulkan | empty, warm | 3.54 s | 1,561 in 0.12 s | 6.38 s | 1,232 MB | 328 MB |
| Vulkan | pack without pipelines | 3.53 s | 0 | 6.21 s | 859 MB | 287 MB |

About 2.6 s of every run is the game's own start (window, graphics device, the executable loaded). A normal D3D12 start on that storage with the driver's cache cold: nothing missing (1 ms), then 2,928 pipelines in 1.32 s before the logos.

**Retiring an old pack's entries: done on 2026-10-25** (the last section). Until then the import only added: an entry of an older pack that a newer pack no longer had stayed, created at every start, about 85 KB of process memory each on D3D12, 240 KB and 26 KB of video memory on Vulkan (from the figures above).

**Frame generation, again** (`fg/present-gate.pad`, the clean build): through AMD's swap chain, HDR, 4,800 of 4,800 guest frames presented once in the 40 s drive, none dropped.

## The second review's fixes (2026-10-24)

The fixes for the second review (`dc1a31a`: N1 to N6 on the game side, LM1 to LM3 and the launcher's lows). Each was run against the build before it (`eaf3ad4`) and after, on scratch user-data and cache roots (never `Documents\mcla`), with the stamps and storage of a real preparation.

**Game side** (`src/shader_prepare.cpp`, `src/shader_storage_files.*`)
- **N1, the rollback cut the player's own entries.** The journal now records, per file, the length after the import and the XXH3 of the added part. The next start cuts a file back (or removes one the import created) only when it still is what the import left: its length is the recorded one and the added part hashes the same. Otherwise the file is left alone, the journal cleared, and the log says so. A journal from an earlier build (no hash) is trusted by length alone. Test: a start crashed after its import, the player's own shader and pipeline entries appended (the start that "played" and only lost the `done` record): before, both files were cut back to the lengths before the import and the pack imported again, which lost the two entries (48 and 72 bytes: the files ended at 379,640 and 210,828 bytes instead of 379,688 and 210,900); after, both kept whole. Same length with a byte of the added part changed: kept. Untouched files (a crash during the preload): cut back or removed as before (5 files), then imported again.
- **N2, stale temporaries.** The callers now pass complete names (`545407F8.xsh`, `545407F8.import`, ...). Test: 5 temporaries of the forms `<name>.<digits>.tmp` and `<name>.prepare.tmp` in both folders; before, none removed; after, all 5, and 7 look-alikes (`.bak.tmp`, `.12a.tmp`, another title, the lock's) kept.
- **N3, exit 7 for permanent failures.** `WriteFileAtomically` no longer maps every lasting `ERROR_ACCESS_DENIED` to "in use", and a read-only file isn't waited for. `MoveFileExW` also answers access denied when another program has the target open without delete sharing or mapped (an antivirus scan, OneDrive, a backup), which passes; so a lasting access denied on a file that isn't read-only is told apart with a probe, `CreateFileW(path, DELETE, all sharing)`: a sharing violation or a success is the other program (7), access denied is the permission (6), anything else keeps the error. `AcquireSessionLock` returns `kTaken`, `kHeld` or `kError`: only a held lock (or a sharing violation opening the lock file) is 7; a lock file that can't be opened or locked is 6. Tests (prepare): a read-only list or shader file, a folder or a read-only file in place of the lock file, and delete denied by an ACL on the list file and on its folder (`icacls /deny (DE)` and `(DC)`; `(DE)` alone doesn't block, the folder lets its owner delete): exit 6 (the build before the fixes gave 7 for the read-only file; the ACL case was run only on builds with the fix), the first file already replaced put back, no temporary left; a lock held by another process, the lock file or a list file open without sharing, and a list file open the way Python's `open()` does (read and write sharing, no delete): exit 7. The first version of this fix (`c920bfd`) gave 6 for the last one (measured: exit 6 in 4 s); the probe is what makes it 7. A file mapped by another program (a data view, its handle closed) did not stop the replacement at all here. A normal start with a read-only file or an unusable lock goes on without an import.
- **N4.** `--mcla_prepare_shaders_retry` also forgets the shaders recorded as not rebuildable (and the failure count), not only a disabled import. Test: a journal marking 2 shaders unbuildable: `start 421` without the option, `start 423` with it, and the line gone from the journal. That interrupted runs (a kill, the launcher closing, a power cut) count as failed starts is now documented in the source and in `KNOWN-ISSUES.md`.
- **N5.** The wait for a preparation's lock (up to two minutes, on the window's thread) now looks at the message queue every 100 ms (`PeekMessage` with `PM_NOREMOVE`), and an empty owner text (the holder rewriting it) is read again instead of skipping the wait. Test: a start while another process holds the lock as "prepare" for 22 s: the window was "not responding" (`IsHungAppWindow`) from the 6th second before; never after. The window still isn't painted meanwhile. A first attempt with a message range nothing uses did not help; looking at all messages does.
- **N6.** The no-lock branch is `#if !REX_PLATFORM_PS5` (the SDK's own platform macro, `<rex/platform.h>`), flock everywhere else. **Not compiled or run**: the PS5 build can't be made here, and the Linux branches weren't compiled either; the Windows branches are the ones tested.

**Launcher** (`ShaderStamp.cs`, `ShaderPrep.cs`, `LauncherApp.cs`; the logic through `launcher-check` against stamps from real runs, edited; the window through UI Automation on the launcher built into the clone, with scratch roots in its options)
- **LM1.** The API and the resolution scale are compared with the stamp's `settings` (what the last start was asked for), not with `storage` (what it got). A scale of 3 that the game clamped to 1 and a Vulkan request that fell back to D3D12 no longer prepare at every Play; a real change still does. A request for "any" after an explicit API prepares once more (the launcher can't tell what "any" resolves to), and an explicit API after "any" is compared with the one "any" came to.
- **LM2.** With `mcla_shader_pack_auto = false` the pack is compared only when the stamp says the last run imported it (`pack_import` "on", a preparation).
- **LM3.** (Checked in the window: before Play, Stop ends the run and starts nothing and a second Play starts the game without preparing again; Play now ends the run and the game starts and leaves the journal at `done`; closing the launcher during the run ends it and starts nothing.) While a preparation runs the buttons are **Stop** (ends it, starts nothing, and Play doesn't ask for another until the launcher is opened again or *Prepare shaders* is used) and **Play now** (ends it and starts the game). Both are safe: the storage is only replaced whole and the game imports by itself at its start (the interrupted run counts as a failed start in the journal, which the next start that gets through resets).
- **Lows.** A run the player (or the launcher closing) ended no longer sets the skip mark, and a failed run by hand sets it like an automatic one (before, any run by hand cleared it); the mark still silences every reason for that pack, on purpose: the failure was shown when it happened, and every reason would start the same failing preparation. Closing (or restarting to update) during a preparation no longer starts the game. A custom `--mcla_shader_pack` is the pack compared (relative paths against `mcla.exe`'s folder, as the game's working folder). Relative `cache_root` and `user_data_root` are taken against that folder too. Those two are read from the options and the `REX_` variables only, because the SDK reads them before `mcla.toml` is loaded (the launcher used to look in the toml). A malformed folder option ends the check without a preparation instead of throwing. Not fixed, by design: a graphics API left on "any" that the game resolves differently, a switch between two adapters that both stay installed, a storage format change without a new pack (no preparation fixes that one); each is in the header of `ShaderStamp.cs`.

## Growing the pack and retiring stale entries (2026-10-25)

Two jobs: the pack (2026-10-17) came from the agents' sessions and the owner's own, so places nobody had been to still compiled while playing; and an entry a newer pack drops stayed in every storage for good. The tools are `launcher/dev/shaderpack/coverage.py`, `matrix.py`, `route.py`, `measure.py` and `test_pack.py`, with `dev/scripts/arcade-race.pad` and the tours (`launcher/README.md` says how they are used). Everything ran on scratch user-data and cache roots under `C:\mcv\shp-dev` (never `Documents\mcla`), through `devrun.py`, which plays on a private copy of the save (the only use of the real save, its hashes compared before and after); the pack holds the same kind of data as before (fingerprints, offsets and lengths, pipeline descriptions) and no new kind of data came from the disc.

### Method

- **A soak** (`coverage.py soak`): a controller script on a new, empty storage with the pack imported at the start, as a player's is after the launcher's step. What the game creates while playing, the pipelines and shaders the storage holds afterwards that the pack did not, is what the pack missed: each was a draw left out while it compiled.
- **The races** (`matrix.py`, 38 arcade races: 18 ordered, 14 circuit, 6 landmark): a probe run per race records the leading opponent's route from the cars' positions in the log (`watch 15`, `route.py from-log`); then each race runs on D3D12 `rtv` and on Vulkan `fbo` with the car following that route at 0.85 of the leader's speed (`follow`, which brings it back when it is knocked off: four reverses, a forward burst, the nearest route point behind or ahead, a tip's dialog dismissed by A), the race list and options menus, the pause menu, the map and the leaderboards toured, dynamic weather and time of day, 3 opponents, traffic off. For 10 of the races a run with traffic on, per API, and one free-roam soak per API (a long circle in the city with time of day and weather moving on).
- **Totals:** 136 runs, 473 minutes, 4,165,253 frames (38 probes, 76 race runs, 20 with traffic on, 2 roam); 7 of them failed (below).
- **The pack:** `coverage.py grow` gives `build_pack.py` all the runs' storages with `--previous` (it refuses a pack that lacks an entry of the old one for no stated reason) and `--check` (`mcla.exe` rebuilt every shader). `rov` and `fsi` were not played: their entries are the old pack's.
- **The display:** the shared GPU lock lets the display sleep during batch runs (it is an OLED); a coverage count (pipelines and shaders created) does not depend on it, and nothing below that was timed was measured with the display off: the preparations were run with it kept on.

### What the old pack missed

| | old pack | grown pack |
|---|---|---|
| Shaders | 723 | 762 |
| D3D12 `rtv` pipelines | 2,928 | 3,111 (+183) |
| D3D12 `rov` pipelines | 570 | 570 |
| Vulkan `fbo` pipelines | 1,561 | 1,721 (+160) |
| Vulkan `fsi` pipelines | 474 | 474 |
| Pipeline descriptions in all | 5,533 | 5,876 |
| Size | 411,416 bytes | 436,640 bytes (+6.1%) |
| The check | 723 of 723 rebuilt | 762 of 762 |

- Nothing of the old pack was dropped (`grown-a1.retired.json`: no shader, no description).
- Per run on the old pack: 0 to 65 pipelines created in play (the first races found the most; after the first 15 most runs found 0 to 11, but a few later ones still found 20 to 30: `c10` with traffic on, `l00` on Vulkan).
- Traffic on found 28 `rtv` entries that no run with traffic off did, and none on `fbo` (10 races each).
- **What a miss cost on the old pack** (warm driver cache, in the lines the dev build counts, `visible` in each soak's result): on D3D12, ordered race 5, 9 pipelines created in play: no draw left out, 6 made on the spot, 4.8 ms of the frame budget waited; on Vulkan `fbo`, 19 created: 66 draws left out over 33 frames, 13 presents skipped, 173 ms waited. With a cold driver cache, ordered race 3 on D3D12, 26 created: 235 draws left out, 266 ms waited. Over all 136 runs: 1,809 draws left out, 2,015 made on the spot, 557 ms of pipeline work on the GPU thread, 259 presents skipped.
- **With the grown pack** the six baseline scripts (`boot-to-roam`, `drive-spawn` and `race.pad`, on both APIs, the same runs as the old pack's) create nothing in play, none of the visible counts above is more than 0, and the frames over 50 ms are 13 (3 over 100 ms, the worst 111 ms) in 33,551 frames, against 19 (3 over 100 ms, the worst 470 ms) in 33,550 with the old pack.

### What this does not show

- **No hold-out was run.** The grown pack was made from the 136 runs, and the runs that tested it were the old scripts, which the old pack already covered. How many of the draws a player would see left out in conditions the soaks did not use (dusk, rain, power ups, other traffic, career events) it saves is not measured; the 343 new pipelines are what the soaks showed the old pack lacked. `matrix.py` writes a hold-out (a third variant of every third race), `coverage.py compare` says what it needed beyond each pack, and `batch --only` runs a subset: not run, because the runs take hours.
- **Not played:** `rov` and `fsi` (so those paths gain nothing here), the second variant of each race (noon, clear, power ups, one more opponent), traffic in most races.
- **Races that did not complete (7 of 136):**
  - `o15` (both APIs) and `o05`: a tool bug. A single reading of the log in which every opponent is 600 to 1,100 m away, and the next one is back, was taken for a reset, which cut the first part off the route: `o15`'s started 713 m from the grid (the follower never reached it, 0 to 6% covered; `o15` and `o16` are nearly the same 7.7 km route and `o16` completed), `o05`'s 1,154 m (its runs drove the last 2.7 of 6.2 km, and passed). Fixed in `route.py` (a reading far off that the next undoes is skipped; `test_pack.py` tests it); the routes were remade and the runs on them were not repeated.
  - `o01` (66% of the route on both APIs), `o13` (45% and 42%) and `o09` (34% on D3D12; it completed on Vulkan): the whole world stands still ("a tip or a dialog") at a point of the route and ten presses of A do nothing; nothing frees the car. What the dialog is was not found out (only the last picture of each run was kept). The pack holds what they found before that; the rest of those routes, about 34 to 55% of them, was not driven.
- **Unreachable by these scripts:** career events and their cutscenes, the garage, dealers and other players' cars, the powers Zone, Agro and Roar (not seen firing), police (not seen), the Race Editor and everything online, cheats that need collectables; the save is at the start of the career. These stay as they were: compiled the first time they are drawn.

### The preparation with the grown pack

The same session and build, one preparation per cell on a new empty storage with a new driver-cache salt, then again on it (`prep3.py`, `measure.py prepare --twice --pack`, the display kept on); "process" is from the process's creation to its report, memory at the end with every pipeline created.

| API, path | Pack | Cold: rebuild | Cold: pipelines | Cold: process | Cold: memory | Warm: process | Warm: memory | Video |
|---|---|---|---|---|---|---|---|---|
| D3D12 `rtv` | old | 3.58 s | 2,928 in 2.31 s | 6.41 s | 1,110 MB | 0.62 s | 991 MB | 238 MB |
| D3D12 `rtv` | grown | 3.59 s | 3,111 in 2.34 s | 6.40 s | 1,201 MB | 0.63 s | 997 MB | 238 MB |
| Vulkan `fbo` | old | 3.49 s | 1,561 in 8.04 s | 12.13 s | 1,347 MB | 0.68 s | 1,170 MB | 328 MB |
| Vulkan `fbo` | grown | 3.67 s | 1,721 in 8.58 s | 12.85 s | 1,421 MB | 0.69 s | 1,186 MB | 328 MB |
| D3D12 `rov` | old | 3.63 s | 570 in 2.33 s | 6.43 s | 1,040 MB | 0.60 s | 967 MB | 238 MB |
| D3D12 `rov` | grown | 3.69 s | 570 in 2.38 s | 6.54 s | 1,033 MB | 0.62 s | 974 MB | 238 MB |
| Vulkan `fsi` | old | 3.49 s | 474 in 7.70 s | 11.84 s | 1,397 MB | 0.66 s | 1,109 MB | 334 MB |
| Vulkan `fsi` | grown | 3.66 s | 474 in 7.74 s | 11.99 s | 1,446 MB | 0.66 s | 1,121 MB | 334 MB |

- The grown pack rebuilds 39 shaders more (0.01 to 0.18 s more here) and creates 183 more pipelines on D3D12 `rtv` (+0.03 s) and 160 more on Vulkan `fbo` (+0.54 s); a normal start with the driver's cache warm is as before. The warm memory is 6 MB more on D3D12 and 16 MB more on Vulkan (33 and 100 KB a pipeline; the 85 KB of the old figures is the average between them). The pack file is 25 KB larger and is read at every start.
- One cell each, so a few percent is noise. An earlier single run of D3D12 `rov` (2026-10-24) took 11.5 s for its 570 pipelines and 1,416 MB; today's two took 2.3 s and about 1,035 MB. That is not explained (a driver cache that is warm for those shaders despite the salt is the likely reason), so read the D3D12 "cold" cells as "new salt", and compare the packs within this table only. The 2026-10-17 tables above are from other days on the same machine (D3D12 `rtv` 1.2 s for its pipelines then, 1.7 s on 2026-10-24, 2.3 s here).

### Retiring stale entries

Until now the import only added: an entry of an older pack that a newer pack dropped stayed in the player's storage for good, created at every start. This adds what the 2026-10-17 note said was missing: a record of which entries each pack added, a removal that never touches the player's own, and a way to undo it (here: nothing to undo, by how it is ordered).

**What counts as stale, and who says so**

- A **pack** (`launcher/dev/shaderpack/build_pack.py`) may lack an entry of the pack it replaces only for a reason the tool can state: the shader has no source in the game's files, or `mcla.exe` could not rebuild it in the check (a description goes with a shader it uses), or `--drop <file>` names it with a reason (`<16 hex digits> <why>` per line: a shader, or a description of any list). Every other absence (a storage the maintainer forgot to give, a shorter run) REFUSES the pack: `--previous <pack>` compares, `--report` writes what went and why. A pack can grow without a reason; it cannot shrink without one. Entries of another storage version are not carried at all (the old pack is refused by the game with exit 5, the SDK empties such files, and a pack for the new versions is made from storages of the new versions).
- A **storage** retires an entry only when an earlier pack's import *added* it (the ledger below says so) and the pack now in use no longer has it.

On this build, against the pack of 2026-10-17: nothing in it is stale by those rules. All 723 shaders rebuild (the check), and every description of every list is created at the preload (D3D12 `rtv` 2,928 of 2,928, `rov` 570 of 570, Vulkan `fbo` 1,561 of 1,561, `fsi` 474 of 474), so the grown pack drops nothing and there was nothing to retire from the pack itself. The code is there for the pack that does drop something.

**The ledger** (`<cache>\shaders\local\545407F8.ledger`, binary, with its XXH3): the pack whose import last got through the creation of the stored pipelines, the storage versions, and the shaders and descriptions an import *added*, the ones the storage did not have. What the storage held before an import (the player's own, an earlier build's imports), and what the game wrote itself, is never in it. It is written at the end of a start (`MclaShaderStorageAfterPreload`) only when it changes. A ledger that is missing, damaged, or for other storage versions means "retire nothing" (a new one is made at the next import); one that can't be read at the moment is left alone and nothing is retired that start.

**The retirement** (`RetireStale`, called by the import before it plans what is missing) runs at a start, before the SDK opens the files, when
- the ledger is the pack in use's (its import went through at an earlier start: a pack never retires anything at the start that imports it, so an import that is rolled back, disabled or interrupted meets no removal, and the rollback's lengths stay true),
- the journal says `done` (no failed start since), and
- less than half of what imports added would go (`retire-refused` in a preparation, a warning in the log, otherwise; the preparation of More > Prepare shaders, `--mcla_prepare_shaders_retry`, may retire more: a pack that lacks most of the one before is a mistake or another pack).

It removes the ledger's descriptions the pack lacks, and the ledger's shaders the pack lacks that no description left in any of the four lists uses (a player's own description keeps its shader, and the shader stays listed). Lists are replaced first and the shader file last, each atomically (a temporary file, flushed, renamed), so a start that ends in between leaves a storage that is valid, and the next start finishes; nothing is added or moved, what stays keeps its order, the ledger loses what went. A failure while replacing (a file in use, a read-only file) is logged and the next start goes on. The log and, in a preparation, the standard output (`SHADERPREP retire <shaders> <descriptions>`) say what went and how long it took.

So the order for a pack that drops entries is: start 1 imports it and, once its pipelines are created, the ledger names it; start 2 retires. (The launcher's preparation after an update is start 1, the first Play is start 2.) Entries from before the ledger existed are never retired: they can't be told from the player's own.

**Tests** (`test_storage.py`, through `devrun.py` on scratch roots; the whole file, 35 checks, passes on the grown pack in 190 s, and `test_pack.py`, the pack tools' own tests that need no game):
- R1: a start with a pack on an empty storage writes the ledger (the pack, 762 shaders, 3,111, 570, 1,721 and 474 descriptions).
- R2: the player's own entries (a description with its shader, and one whose shader the import brings) are not in the ledger; the start that first sees a smaller pack retires nothing; the next start retires what the pack dropped (132 descriptions and 1 shader), in order and nothing else; the player's own description and its shader stay, so does a dropped shader something uses, a dropped shader nothing uses goes; the ledger loses what went and then nothing more is retired.
- R3: a pack that lacks most of what imports added retires nothing and the log says why; `--mcla_prepare_shaders_retry` in a preparation retires them (`SHADERPREP retire 662 5735`).
- R4: no ledger: nothing retired, a ledger is made for the pack in use. R5: a damaged ledger is ignored and made again. R6: a start that ended after the lists and before the shader file is finished by the next.
- In `test_pack.py`: what a pack may lack of the previous one (a shader without a source, one that did not rebuild, `--drop`; anything else refused).

**Not verified:** the retirement code is platform-neutral (the same file functions as the import's) but the Linux and PS5 builds were neither compiled nor run; the PS5 build uploads the pack as before.

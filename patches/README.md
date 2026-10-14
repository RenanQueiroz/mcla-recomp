# Patches to the ReXGlue SDK

Apply to ReXGlue v0.10.0 (`f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`), from the SDK checkout:

```
git apply ../../patches/rexglue-v0.10.0-mcla.patch
git apply ../../patches/rexglue-minmax-blend-factor.patch
git -C thirdparty/FFmpeg apply ../../../../patches/rexglue-ffmpeg-ps5-config.patch
git apply ../../patches/rexglue-gpu-regwrite-bulk.patch
git apply ../../patches/rexglue-imgui-dialog-repaint-on-demand.patch
git apply ../../patches/rexglue-inline-bit-helpers.patch
git apply ../../patches/rexglue-perf-counters-batched.patch
git apply ../../patches/rexglue-shared-memory-request-fast-path.patch
git apply ../../patches/rexglue-threaded-submission-d3d12.patch
```

The first was several files at first; they are combined now because several touch the same build files and would not apply independently. The second goes on top of the first (both change `src/graphics/vulkan/command_processor.cpp`). The build scripts apply all of them, and add the second and the ones after the FFmpeg patch to an SDK checkout that was patched before they existed. Those later patches go on in the order listed, as several change neighbouring lines.

## rexglue-v0.10.0-mcla.patch

| Change | Files | Affects |
|---|---|---|
| Vblank timer resync: the guest tick count can step backwards, which made an unsigned difference wrap and fire vblank interrupts without end | `src/graphics/graphics_system.cpp` | all platforms |
| Per-frame counters kept in Release builds, and their CSV path taken from `REX_PERF_LOG_CSV` (the GPU plugin has its own copy of the logger state, so a path set by the host never reached it) | `CMakeLists.txt`, `src/core/CMakeLists.txt`, `src/system/CMakeLists.txt`, `src/core/perf/counter.cpp` | all platforms |
| Debug-only register lookup removed from every GPU register write in Release builds | `src/graphics/command_processor.cpp` | all platforms |
| PS5 platform: everything else. Platform detection, libc and libc++ differences, anonymous shared memory with a shared-object address reservation at a kernel-chosen base, 16 KiB host pages, signals without realtime signals, no X11/Wayland/RenderDoc, static runtime and GPU libraries, no desktop install rules | the remaining files | PS5 only, behind `REX_PLATFORM_PS5` or `if(PS5)` |

Details of the PS5 changes are in `docs/ps5-port-plan.md`.

## rexglue-minmax-blend-factor.patch

The car reflection fix: cars came out white with a glow around them, worst at night. The Xbox 360's GPU applies the source blend factor in the MIN and MAX blend equations; Direct3D 12 and Vulkan ignore blend factors there. The game draws the light glows in the cars' environment map with `max(source x SrcAlpha, destination)`, so every glow became a solid square, and the paint mirrored those. With the patch the translated pixel shader multiplies by its own alpha in that case. Cause, evidence and limits: `REFLECTION_FIX.md`.

| Change | Files | Affects |
|---|---|---|
| System flags per render target, set when its colour or alpha blend is MIN or MAX with source factor SrcAlpha and destination factor One | `src/graphics/d3d12/command_processor.cpp`, `src/graphics/vulkan/command_processor.cpp` | D3D12; Vulkan (PC, PS5) |
| The multiply by the saturated alpha before the colour is written | `dxbc_translator.h`, `dxbc_translator_om.cpp`, `spirv_translator.h`, `spirv_translator_rb.cpp` (under `include/rex/graphics/pipeline/shader` and `src/graphics/pipeline/shader`) | D3D12; Vulkan (PC, PS5) |

Host render target path only (the default on both backends).

## rexglue-ffmpeg-ps5-config.patch

FFmpeg selects a configuration header per platform. This adds `config_ps5_x86_64.h`, generated from the Linux x86-64 one with `HAVE_MALLOC_H`, `CONFIG_ICONV`, `HAVE_MEMALIGN`, `HAVE_SCHED_GETAFFINITY` and `HAVE_LINUX_PERF_EVENT_H` turned off, and selects it for `__PROSPERO__`.

## rexglue-gpu-regwrite-bulk.patch

GPU register writes in bulk, the approach of xenia-canary. Every register a command packet wrote was one virtual `WriteRegister` call and a switch, and a range that crossed a shader-constant block fell back to that path for all of it. Ranges are now walked block by block: ordinary registers are copied at once, float and fetch constants are checked against the current shaders' constant usage before anything is invalidated, and only the few registers with side effects (scratch writeback, `COHER_STATUS_HOST`, the gamma ramp) take the slow path. Same registers, same order. Cvar `gpu_regwrite_bulk` (default on; `false` restores the old path, also at run time).

Measured in one run with the cvar switched every 4 seconds, so both sides see the same scenes (Windows, D3D12, RTX 5080 and Ryzen 7 9800X3D, native resolution, uncapped): about 10.5% less time per draw on the GPU command thread, the thread that limits the frame rate. Built and run on Windows only.

| Change | Files | Affects |
|---|---|---|
| Registers with side effects split out of `WriteRegister`; type-0 packets as ranges | `include/rex/graphics/command_processor.h`, `src/graphics/command_processor.cpp` | all platforms |
| Ranges written block by block; float and fetch constants in bulk | `include/rex/graphics/d3d12/command_processor.h`, `src/graphics/d3d12/command_processor.cpp` | D3D12 |

## rexglue-imgui-dialog-repaint-on-demand.patch

An ImGui dialog can say that it needs no repaints of its own (`ImGuiDialog::RepaintsContinuously()`, default true, so other dialogs behave as before; `REX_UI_IMGUI_DIALOG_REPAINT_ON_DEMAND` tells code that it exists). While only such dialogs are shown, the drawer stops asking the presenter for a paint after every paint, and they are drawn with the paints that happen anyway, such as each new guest frame. The achievement toast (`src/achievement_toast.h`) is one: without this, a toast on screen made the presenter repaint at the monitor's refresh rate from the UI thread, on the queue the GPU command thread submits to. Windows, D3D12, 60 cap, while a toast shows: 240 presents a second and 65 to 83% of frames within 1 ms of the target before, 60 and 99.4 to 100% after. Built and run on Windows only.

| Change | Files | Affects |
|---|---|---|
| `RepaintsContinuously()` | `include/rex/ui/imgui_dialog.h` | all platforms |
| Repaint requests only while a dialog that wants them is shown | `src/ui/imgui_drawer.cpp` | all platforms |

## rexglue-inline-bit-helpers.patch

`lzcnt`, `tzcnt` and `bit_scan_forward` are single instructions that the GPU command thread uses on its hot path, but they were defined out of line in `rexruntime.dll`, so each use was a call across DLLs (`bit_scan_forward` alone was 2.5% of that thread in a profile). They are defined inline in `rex/math.h` now, for MSVC-style and GCC-style compilers. Built and run on Windows (Clang) only.

| Change | Files | Affects |
|---|---|---|
| Inline definitions | `include/rex/math.h` | all platforms |
| Out-of-line definitions removed | `src/core/math_msvc.cpp`, `src/core/math_gcc.cpp` | all platforms |

## rexglue-perf-counters-batched.patch

The GPU command thread's per-event counters (draws, vertices, texture and pipeline cache hits and misses, command-buffer stalls) were an atomic add through a call into `rexruntime.dll` for each event. They now add up in plain variables of the calling module and reach the shared counters once a frame, just before the counters' per-frame snapshot. Cvar `gpu_batch_perf_counters` (default on). Measured in one run with the cvar switched every 4 seconds (Windows, D3D12, native, uncapped): about 0.8% less GPU command thread time per draw. Built and run on Windows only.

| Change | Files | Affects |
|---|---|---|
| `AddBatched`, `FlushBatchedCounters` | `include/rex/perf/counter.h` | all platforms |
| Flush before the per-frame snapshot | `src/graphics/command_processor.cpp` | all platforms |
| Counters batched where they are counted | `src/graphics/d3d12/command_processor.cpp`, `pipeline_cache.cpp`, `texture_cache.cpp` | D3D12 (the Vulkan backend does not count these) |

## rexglue-shared-memory-request-fast-path.patch

`SharedMemory::RequestRanges` batched, sorted and took the global lock for every request, also when every page it asked for was valid already, which is nearly always. Such a request now returns before any of that. Cvar `shared_memory_request_fast_path` (default on); not taken while the hot-page options (`shared_memory_hot_page_faults`, used on PS5) are on. Measured in one run with the cvar switched every 4 seconds (Windows, D3D12, native, uncapped): about 2.4% less GPU command thread time per draw. Built and run on Windows only.

| Change | Files | Affects |
|---|---|---|
| Early return when all requested pages are valid | `src/graphics/shared_memory.cpp` | all platforms, both backends |

## rexglue-threaded-submission-d3d12.patch

Direct3D 12 command lists are replayed and submitted on a thread of their own ("GPU Submission"). The GPU command thread only records into the deferred command list; at the end of a submission the recording and its command allocator go to the worker, which resets the command list, replays the recording into it, closes it, executes it and signals the submission fence. While threaded, a submission is also ended every 1,000 draws (`d3d12_threaded_submission_split_draws`, 0 for never) so that replay overlaps recording; not while an occlusion query is active or pipelines are being created. Allocators are reused only after their fence, which the worker signals after submitting. Anything that has to see earlier work in the queue first (tile mapping updates, clearing the allocator cache, shutdown) lets the worker finish, and the swap waits until its frame is in the queue, so presentation latency does not grow. Cvar `d3d12_threaded_submission` (default on; can change at run time). The design is from ForzaHorizon1-ReImagined's threaded submission for Vulkan (MIT); the code here is written for this backend.

Before this, replaying the recording took 23 to 41% of the GPU command thread at native resolution. Measured in one run with the cvar switched every 4 seconds (Windows, D3D12, RTX 5080 and Ryzen 7 9800X3D, uncapped): 15.1% less GPU command thread time per draw at native resolution (95 to 111 FPS on a heavy view; the split alone 7.1%), 4.7% at three times the resolution, where the GPU limits. 60 FPS pacing unchanged (99.4% of frames within 1 ms). Stress: three 15-minute runs (cvar switched every 5 seconds; achievement toasts; minimize and restore; about 110 fullscreen switches at 3x) and runs with the D3D12 debug layer, also with GPU-based validation: no errors from this change and no hangs. Built and run on Windows only.

| Change | Files | Affects |
|---|---|---|
| Submission thread, split submissions | `include/rex/graphics/d3d12/command_processor.h`, `src/graphics/d3d12/command_processor.cpp` | D3D12 |
| Recording handed over whole | `include/rex/graphics/d3d12/deferred_command_list.h`, `src/graphics/d3d12/deferred_command_list.cpp` | D3D12 |
| Wait for the worker before tile mapping updates | `src/graphics/d3d12/shared_memory.cpp`, `src/graphics/d3d12/texture_cache.cpp` | D3D12 |

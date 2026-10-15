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
git apply ../../patches/rexglue-timer-queue-blocking-wait.patch
git apply ../../patches/rexglue-shared-memory-fast-path-hot-pages.patch
git apply ../../patches/rexglue-hot-pages-watched-ranges.patch
git apply ../../patches/rexglue-cursor-auto-hide.patch
git apply ../../patches/rexglue-vulkan-memexport-check-order.patch
git apply ../../patches/rexglue-vulkan-hoist-shared-memory-uploads.patch
git apply ../../patches/rexglue-threaded-submission-vulkan.patch
git apply ../../patches/rexglue-d3d12-placed-texture-heaps.patch
git apply ../../patches/rexglue-vulkan-texture-load-storage-copy.patch
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

Measured in one run with the cvar switched every 4 seconds, so both sides see the same scenes (Windows, D3D12, RTX 5080 and Ryzen 7 9800X3D, native resolution, uncapped): about 10.5% less time per draw on the GPU command thread, the thread that limits the frame rate. On Vulkan, which gets the common part (the backend's `WriteRegistersFromMem` already took single constant blocks in bulk), 9.1% less frame time per draw (plus or minus 1.0%, cvar switched every 8 seconds, native, uncapped). Most of the gain is the common part: switched on its own, the block walker measured 1.7% (plus or minus 1.0%) less GPU command thread time per draw on D3D12, and a port of it to Vulkan 0.5% (plus or minus 0.6%), so the Vulkan port was left out (`launcher/dev/dropped-patches/rexglue-vulkan-regwrite-walker.patch` in the fork). Built and run on Windows only.

| Change | Files | Affects |
|---|---|---|
| Registers with side effects split out of `WriteRegister`; type-0 packets as ranges | `include/rex/graphics/command_processor.h`, `src/graphics/command_processor.cpp` | all platforms, both backends |
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

The Vulkan backend counted none of these, so `REX_PERF_LOG_CSV` showed 0 draws on Vulkan and nothing could be compared per draw there. It now counts the same events, batched the same way: draws and vertices in `IssueDraw`, blocking fence waits as command-buffer stalls, pipeline lookups, and image view lookups as texture cache hits and misses (the counterpart of the D3D12 descriptor lookups).

| Change | Files | Affects |
|---|---|---|
| `AddBatched`, `FlushBatchedCounters` | `include/rex/perf/counter.h` | all platforms |
| Flush before the per-frame snapshot | `src/graphics/command_processor.cpp` | all platforms |
| Counters batched where they are counted | `src/graphics/d3d12/command_processor.cpp`, `pipeline_cache.cpp`, `texture_cache.cpp` | D3D12 |
| The same counters on Vulkan | `src/graphics/vulkan/command_processor.cpp`, `pipeline_cache.cpp`, `texture_cache.cpp` | Vulkan (PC, PS5) |

## rexglue-shared-memory-request-fast-path.patch

`SharedMemory::RequestRanges` batched, sorted and took the global lock for every request, also when every page it asked for was valid already, which is nearly always. Such a request now returns before any of that. Cvar `shared_memory_request_fast_path` (default on); with hot pages or widened requests on (the PS5's settings), see `rexglue-shared-memory-fast-path-hot-pages.patch`. Measured in one run with the cvar switched every 4 seconds (Windows, D3D12, native, uncapped): about 2.4% less GPU command thread time per draw. Built and run on Windows only.

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

## rexglue-timer-queue-blocking-wait.patch

The SDK's timer queue thread waited for the next timer by spinning, which kept about half a core busy for the whole session. It now sleeps on a condition variable until the next timer is due or a new one is queued; publishing a timer always signals that side, so the choice can change while the thread waits. This needed a fix in disruptorplus' blocking wait strategy, whose `wait_until` passed its arguments to the condition variable in the wrong order. Cvar `timer_queue_blocking_wait` (default on). No change in frame rate; less CPU time, power and heat. Built and run on Windows only.

| Change | Files | Affects |
|---|---|---|
| Blocking wait for new timers | `src/core/timer_queue.cpp` | all platforms |
| `wait_until` argument order: (lock, time, predicate) | `thirdparty/disruptorplus/include/disruptorplus/blocking_wait_strategy.hpp` | all platforms |

## rexglue-shared-memory-fast-path-hot-pages.patch

The fast path of `rexglue-shared-memory-request-fast-path.patch` was not taken while hot pages (`shared_memory_hot_page_faults`) or widened requests (`shared_memory_request_granularity_log2`) were on, so with the PS5's settings every request (about 1.4 million a second on a heavy view) went through batching, sorting and the global lock. Neither needs that for a request whose pages are all valid: hot pages are left invalid (unless the GPU wrote them, and those are not uploaded on the full path either), so a request that touches one still takes the full path; widening only adds pages around a request that has invalid ones. Cvar `shared_memory_request_fast_path_with_hot_pages` (default on; off restores the old condition). Built and run on Windows only; on the PS5, which runs with hot pages and widened requests, it applies to every frame.

Measured on Windows (D3D12, RTX 5080 and Ryzen 7 9800X3D, native, uncapped, hot pages on with `--shared_memory_hot_page_faults=4 --shared_memory_hot_page_ms=10000`, cvar switched every 20 seconds in one run): full-path requests 1.45 million a second against 110,000, and GPU command thread CPU time per frame 6.1-6.5 ms against 4.9-5.4 ms (about 17% less; 4.4 against 3.8 ms on a lighter view). Without hot pages it changes nothing.

On a PC, with this patch, the hot-page settings above (off by default) cut the GPU command thread's CPU time per frame by about 20% against the defaults (7.0 to 5.6 ms on a heavy view, in-run switch every 20 seconds): write faults went from about 12,000 a second to about 200, page protection changes from about 190,000 a second to about 1,000-2,000, and uploads from 1.2-1.8 GB/s to about 0.1 GB/s. A heavy title view went from 122 to 146 FPS.

| Change | Files | Affects |
|---|---|---|
| Fast path also with hot pages and widened requests | `src/graphics/shared_memory.cpp` | all platforms, both backends |

## rexglue-hot-pages-watched-ranges.patch

A fix for hot pages (`shared_memory_hot_page_faults`, on for the PS5). A hot page is not write-protected, so a guest write to it never faults; a request that uploads it tells whatever watches it (`FireWatches`), but a texture is only requested again once a watch has said it changed. A texture loaded from a hot page was therefore never reloaded until the page cooled down (`shared_memory_hot_page_ms`, 2 s by default, 10 s in the settings measured above), and the CPU's later writes to it did not show. The attract movie made it plain: Bink writes its luma and chroma planes into textures every frame, and with hot pages the colour planes kept earlier shots' colours (red and blue blobs, pink outlines of other shots' shapes) over the current picture. `SharedMemory::WatchMemoryRange` now takes the watched pages out of the hot set and write-protects them again (they stay invalid, so nothing is skipped), so the next guest write faults and fires the watch. A page that keeps being written turns hot again after `shared_memory_hot_page_faults` faults, and the next texture load cools it again; pages that only hold vertex and constant data stay hot as before.

Evidence (Windows, D3D12, native, uncapped, `--shared_memory_hot_page_faults=4 --shared_memory_hot_page_ms=10000`): window captures of the whole movie every 60 ms compared with the movie decoded by FFmpeg, after normalising each frame's contrast. Without the fix 21 of 589 captures matched no movie frame (mean difference 0.24); with it none did (mean 0.17, the same as with hot pages off). With the fix, hot pages still made frames about 9% shorter on the title screen (6.87 against 7.57 ms, both halves of an in-run switch every 20 seconds agree). Built and run on Windows only; the PS5, which runs with hot pages, should have the same movie fault and the same fix.

| Change | Files | Affects |
|---|---|---|
| Watched pages are not hot | `src/graphics/shared_memory.cpp` | all platforms, both backends, only with hot pages on |

## rexglue-cursor-auto-hide.patch

The SDK's auto-hidden cursor (`Window::CursorVisibility::kAutoHidden`, inherited from Xenia, used by nothing) made usable for hiding the mouse pointer during play. It used to hide the pointer whenever the delay ran out, focused or not, and only mouse motion brought it back. Now it hides only while the window has focus. The pointer comes back when the window loses focus, and on a click or the wheel as well as on motion. When the window gets focus back, the pointer stays until the mouse has been still for the delay (3.3 s, the SDK's default). Input from a physical pad hides it at once. That signal comes from `InputSystem::GetState`, where the SDK already reads the pads, and the keyboard-and-mouse emulation does not count. A request goes to the UI thread only while the pointer is shown, so polls during play cost an atomic load. While the console, settings or achievements overlay (the ImGui ones driven by the mouse) is open, it stays shown. SDL blanks the pointer only over the client area (`WM_SETCURSOR` with `HTCLIENT` on Windows), so the title bar, the borders and other windows keep theirs. SDL already drops motion events that do not move the mouse, so a cursor change or a window appearing does not bring it back. The hide timer's expiry and the pad's request carry a token that expires with the window, and a timer expiry that mouse input has since replaced is ignored.

Nothing changes unless the app selects `kAutoHidden`. mcla does so with `mcla_hide_cursor` (default on, `src/mcla_app.cpp`).

Checked unattended on Windows, windowed and fullscreen, with `GetCursorInfo` and synthetic input (`SendInput`), 41 checks in four runs, all as expected:
- hidden 4.3 s after a move over the focused game, still shown at 2.4 s;
- a zero-delta move and a same-position `SetCursorPos` leave it hidden;
- a move or a click shows it;
- it shows when focus goes to another window while the pointer stays over the game, and stays shown there;
- after focus comes back, it is shown at first and hidden after the delay;
- it stays shown over a window in front of the focused game, and over its title bar and border;
- it stays shown while the F4 settings overlay is open;
- with `--mcla_hide_cursor=false` nothing changes.

The pad path was checked by reading the code only, as no virtual pad driver is installed. Built and run on Windows only. On the PS5 the window is SDL's offscreen one and the pointer calls do nothing.

| Change | Files | Affects |
|---|---|---|
| Hide only with focus; show on click, wheel and focus changes; ignore stale timer expiries | `include/rex/ui/window_sdl.h`, `src/ui/window_sdl.cpp` | all platforms (SDL windows) |
| `RequestAutoHiddenCursorHide`, `IsAutoHiddenCursorShown`, hide veto | `include/rex/ui/window.h`, `src/ui/window.cpp` | all platforms |
| Pad input hides the pointer | `src/input/input_system.cpp` | all platforms |
| Mouse-driven overlays keep it shown | `src/ui/rex_app.cpp` | all platforms |

## rexglue-vulkan-memexport-check-order.patch

Vulkan's `IssueDraw` asked whether memexport readback is on (`IsReadbackMemexportEnabled`) before it checked whether the draw exported anything, on every draw. The answer comes from `rex::cvar::HasNonDefaultValue("vulkan_readback_memexport")`, a lookup by name in the cvar registry under its lock: 2.5% of the GPU command thread's samples in a profile (Windows, native, uncapped), nearly as much as `IssueDraw`'s own code (2.9%). The ranges are tested first now; a draw that exports nothing (nearly all of them) never reaches the lookup, and one that does gets the same answer as before. D3D12 already asks only for draws that export. Measured in one run with the order switched every 8 seconds (Windows, Vulkan, RTX 5080 and Ryzen 7 9800X3D, native, uncapped, the launcher's settings): 6.1% (plus or minus 0.6%) less GPU command thread time per draw, from the thread's cycle count, and 4.2% (plus or minus 1.5%) less frame time per draw; the Vulkan GPU command thread is busy 97 to 99% of the time at native resolution, so it sets the frame rate there. Built and run on Windows only; on the PS5 (Vulkan) it applies to every draw.

| Change | Files | Affects |
|---|---|---|
| Ranges tested before the setting | `src/graphics/vulkan/command_processor.cpp` | Vulkan (PC, PS5) |

## rexglue-vulkan-hoist-shared-memory-uploads.patch

On Vulkan, copies may not be recorded inside a render pass. Every upload into the shared memory buffer (vertex and index data the CPU rewrote; with hot pages, `--shared_memory_hot_page_faults`, the bytes a draw reads from a hot page) therefore ended the render pass of the draws around it, copied between two barriers and began the render pass again. In MCLA's city that is about 1,650 uploads and 800 render pass begins a frame against about 31 render target changes, measured with GPU timestamps (`launcher/dev/gpu-timestamps-vulkan.sdk.patch`); D3D12 has no render passes and records the same copies in place. An upload of bytes that no draw in the current render pass has read yet is now recorded before the render pass instead, after the barriers that precede it, between one barrier from reads to the copy and one back, which all the uploads hoisted before the same render pass share. Each draw marks the shared memory its vertex fetches, guest indices and memory exports use, a bit per page for the render pass; an upload touching a marked page (or uploaded once already before this render pass) ends the render pass as before. Setting `vulkan_hoist_shared_memory_uploads` (default on).

This is ForzaHorizon1-ReImagined's patch 0036 (Dyotson, MIT licence, <https://github.com/Dyotson/ForzaHorizon1-ReImagined>, `patches/rexglue-sdk/0036-vulkan-hoist-shared-memory-uploads-before-render-pass.patch`), ported to v0.10.0 and extended from page uploads to the byte uploads of hot pages, which are most of MCLA's.

Measured on Windows, Vulkan, RTX 5080 and Ryzen 7 9800X3D, with the launcher's settings, in the scripted timedemo (`launcher/dev/devrun.py`, updates of exactly 1/60 s, uncapped, fullscreen), four runs a side alternating after a warm-up, median frame time:

| Segment | Native, off | Native, on | 3x, off | 3x, on |
|---|---|---|---|---|
| Free roam, standing at the spawn point | 9.39 ms | 6.83 ms (-27%) | 18.89 ms | 15.36 ms (-19%) |
| Drive from the spawn point | 8.57 ms | 7.28 ms (-15%) | 18.40 ms | 15.42 ms (-16%) |
| Map zoom-out (loading) | 7.07 ms | 6.99 ms | 16.15 ms | 14.03 ms |

Free roam at native resolution now matches D3D12 (6.8 ms). Validation (`--vulkan_validation_enabled=true`) shows only the messages it shows without this patch; screenshots at 3x with it on and off show the same picture. Built and run on Windows only; on the PS5 (Vulkan, with hot pages) the same render pass breaks happen.

| Change | Files | Affects |
|---|---|---|
| Hoist point at each guest render pass, hoisted command stream | `include/rex/graphics/vulkan/deferred_command_buffer.h`, `src/graphics/vulkan/deferred_command_buffer.cpp` | Vulkan |
| Pages read in the render pass, hoisting decision | `include/rex/graphics/vulkan/command_processor.h`, `src/graphics/vulkan/command_processor.cpp` | Vulkan |
| Page and byte uploads recorded before the render pass | `include/rex/graphics/vulkan/shared_memory.h`, `src/graphics/vulkan/shared_memory.cpp` | Vulkan |
| The guest index range of the last draw | `include/rex/graphics/primitive_processor.h`, `src/graphics/primitive_processor.cpp` | all (read by Vulkan only) |

## rexglue-threaded-submission-vulkan.patch

The Vulkan counterpart of `rexglue-threaded-submission-d3d12.patch` (same design, from ForzaHorizon1-ReImagined's patch 0038, Dyotson, MIT licence). `EndSubmission` hands the deferred commands (and those hoisted before render passes) to a "GPU Submission" thread with the submission's command pool and fence; the thread records the Vulkan command buffer from them and submits it, while the GPU command processor goes on with the next packets. Settings `vulkan_threaded_submission` (default on) and `vulkan_threaded_submission_split_draws` (default 1000: a submission is also ended after that many draws when it can be, so less is left to record at a swap; 0 measured the same on Vulkan).

Ordering is kept as without the thread: submissions reach the queue in order, a fence is waited on only once the thread has passed it to `vkQueueSubmit` (and only fences of submissions already in the queue are polled), sparse binds and the presenter's own submissions wait until everything before them is in the queue, and turning the setting off drains the thread first. A failed recording or submission on the thread is handled as a lost device (its fence would never be signaled). Also: the Vulkan device is looked up once instead of through the graphics system's virtual `provider()` on every use (1.2% of the GPU command thread's samples in a profile).

Measured on Windows, Vulkan, RTX 5080 and Ryzen 7 9800X3D, with the launcher's settings and `rexglue-vulkan-hoist-shared-memory-uploads.patch`, in the scripted timedemo (updates of exactly 1/60 s, uncapped, fullscreen), four runs a side alternating after a warm-up, median frame time:

| Segment | Native, off | Native, on | 3x, off | 3x, on |
|---|---|---|---|---|
| Drive from the spawn point | 7.22 ms (1% low 112 FPS) | 6.84 ms, -5.3% (1% low 123 FPS) | 15.57 ms | 15.54 ms |
| Free roam, standing | 6.83 ms | 6.85 ms | 15.32 ms | 15.36 ms |
| Map zoom-out (loading) | 6.90 ms | 6.89 ms | 14.81 ms | 13.70 ms, -7.5% |

Standing in free roam the frame rate is held by one of the game's own threads (busy 96 to 98%), and at 3x by the GPU, so the thread shows only where the GPU command thread limits; there it took the GPU command thread from 86% to 60% busy (the submission thread 36%; measured before upload hoisting). Built and run on Windows only; the PS5 notes measure up to 1.5 ms per blocking submit, which this moves off the GPU command thread.

| Change | Files | Affects |
|---|---|---|
| Submission thread, fence and queue ordering, draw-count split | `include/rex/graphics/vulkan/command_processor.h`, `src/graphics/vulkan/command_processor.cpp` | Vulkan |
| Taking and executing the recorded streams on another thread | `include/rex/graphics/vulkan/deferred_command_buffer.h` | Vulkan |

## rexglue-d3d12-placed-texture-heaps.patch

On D3D12 every texture was a committed resource, an allocation of its own: about 0.19 ms of the GPU command thread to create one and 0.08 ms to release one (Windows, RTX 5080, from CPU timers around the calls). Streaming in a new area creates hundreds of textures in a frame, and at 3x the texture cache then goes over its memory limit and evicts over a thousand at once, so the map zoom-out after the title screen had single frames of 150 to 330 ms on D3D12, where Vulkan, whose texture cache sub-allocates from 256 MB blocks with VMA, took 30 to 45 ms for the same frames. Textures are now placed resources in shared 256 MB heaps (heaps for textures that are not render targets or depth buffers, as resource heap tier 1 requires), handed out first fit with freed neighbours merged; a texture over 128 MB is still committed, as VMA does for allocations over half its block size. A texture is only destroyed once the GPU is done with it, as before, so its range goes back to the heap at once; an empty heap is released, keeping one spare. Creating a placed texture measured 8 to 11 microseconds and releasing one under 1. Setting `d3d12_texture_heaps` (default on; `false` makes the textures created from then on committed again).

Measured on Windows, D3D12, RTX 5080 and Ryzen 7 9800X3D, with the launcher's settings, in the scripted city run (`launcher/dev/devrun.py`, `drive-spawn.pad`, fullscreen, uncapped), five runs a side alternating after a warm-up, median of the runs' 1% lows (worst frame):

| Map zoom-out | Native, off | Native, on | 3x, off | 3x, on |
|---|---|---|---|---|
| Real time | 18.9 FPS (158 ms) | 53.9 FPS (44 ms) | 9.1 FPS (239 ms) | 27.3 FPS (47 ms) |
| Timedemo (updates of exactly 1/60 s) | 10.9 FPS (223 ms) | 31.1 FPS (42 ms) | 6.9 FPS (331 ms) | 22.1 FPS (57 ms) |
| Vulkan, for reference (real time) | | 47.9 FPS (45 ms) | | 31.1 FPS (51 ms) |

Free roam and the drive from the spawn point kept their median frame (within 1%), and their 1% lows at 3x rose (the drive's from 56.8 to 63.6 FPS, real time); frame pacing at a 60 FPS cap was unchanged (99.4 to 99.6% of frames within 1 ms). In the race benchmark (`race.pad`, three runs a side) the 1% low went from 50 to 96 FPS at native and from 41 to 61 FPS at 3x, and the worst frame from 102 to 13 ms and from 61 to 20 ms, with the same median. The D3D12 debug layer reports nothing new. Built and run on Windows only; D3D12 only (the PS5 and Linux builds use Vulkan).

| Change | Files | Affects |
|---|---|---|
| Texture heaps, placement and release | `include/rex/graphics/d3d12/texture_cache.h`, `src/graphics/d3d12/texture_cache.cpp` | D3D12 |

## rexglue-vulkan-texture-load-storage-copy.patch

A texture load decodes the guest's data into a scratch buffer with a compute shader and then copies it into the texture's image. On Vulkan the copy was `vkCmdCopyBufferToImage`. On an RTX 5080 with NVIDIA's Windows driver it moved about 120 GB/s; D3D12's `CopyTextureRegion` moved 400 to 850 GB/s for the same textures. These rates come from GPU timestamps around each step of every load (`launcher/dev/texload-split-timestamps.sdk.patch` in the fork). In free roam at 3x, the copies took 3.5 ms of a 15.3 ms GPU frame (D3D12: 0.8 ms); the decode took about 0.8 ms on both APIs. The textures loaded every frame are render targets the game resolved into textures, up to 3840x2160 at 3x. They were the remaining difference between the two APIs at 3x.

A compute shader now writes these textures from the scratch buffer, through an unsigned integer view of the same texel size (R32_UINT, R16G16B16A16_UINT or R32G32B32A32_UINT). The result is bit for bit the same as the copy. A texture qualifies when it:
- has one level;
- is 2D, stacked or cube;
- is at least 64 KiB (`vulkan_texture_load_storage_copy_min_kb`);
- has a 32, 64 or 128-bit colour host format that the device can use as a storage image.

Such images are created with the storage usage and the integer format in their format list. The shader writes every texel of the only level, so the barrier before it discards the old contents (from an undefined layout), and it is kept between two loads of the same texture. Textures with mips keep `vkCmdCopyBufferToImage`; in the city that is one 512x256 texture, about 0.1 ms a frame at 3x.

Setting `vulkan_texture_load_storage_copy`: `auto` (default), `on` or `off`.
- **`auto`** turns it on with NVIDIA's driver only, the only one it was measured with.
- **Other drivers** may not have the slow copy: AMD's, Intel's and Mesa's (the PS5 build uses RADV) copy with shaders of their own. The storage usage could also cost them sampling speed, as some keep storage images uncompressed. Nothing was measured on them; `on` turns it on to try.

Measured on Windows, Vulkan, RTX 5080 and Ryzen 7 9800X3D, with the launcher's settings. GPU timestamps, free roam at 3x, median of nine frames:

| | Off | On | D3D12, for reference |
|---|---|---|---|
| GPU frame | 15.29 ms | 12.73 ms (-16.7%) | 13.99 ms |
| Texture loads | 4.43 ms (copies 3.53) | 1.88 ms (copies 0.96) | 1.59 ms (copies 0.81) |
| Guest draws | 5.37 ms | 5.14 ms | 5.81 ms |

Scripted timedemo (`launcher/dev/devrun.py`, `drive-spawn.pad`; updates of exactly 1/60 s, uncapped, fullscreen). Five runs per configuration, alternating, after a warm-up; median frame time, with the 1% low in brackets:

| Segment | 3x, off | 3x, on | Native, off | Native, on |
|---|---|---|---|---|
| Free roam, standing | 14.91 ms (60 FPS) | 12.04 ms, -19.2% (71 FPS) | 6.84 ms | 6.85 ms |
| Drive from the spawn point | 15.05 ms (59 FPS) | 12.22 ms, -18.8% (71 FPS) | 6.83 ms | 6.80 ms |
| Map zoom-out (loading) | 13.10 ms (28 FPS) | 11.38 ms, -13.1% (31 FPS) | 6.92 ms | 6.89 ms |

At native resolution one of the game's own threads holds the frame rate, so the shorter GPU frame does not show there.

Checks:
- **Validation:** five minutes of Vulkan validation at 3x, driving, with fullscreen switched every 5 s. Only the three kinds of message that this fork's other changes don't cause either: a swapchain destroyed while in use, a pixel shader output with no attachment, and an occlusion query begun outside a render pass.
- **Synchronization validation:** the same report with the setting on and off, a write after write between load dispatches into the scratch buffer.
- **Screenshots at 3x** of the map zoom-out, free roam and the drive with it on and off: the same picture, though the time of day and the traffic differ from run to run.
- **The attract movie:** every capture (about every 150 ms) matches a frame of the movie, as with the setting off.

Built and run on Windows only, with an NVIDIA RTX 5080.

| Change | Files | Affects |
|---|---|---|
| Copy through a storage view, which textures, the setting | `include/rex/graphics/vulkan/texture_cache.h`, `src/graphics/vulkan/texture_cache.cpp` | Vulkan |
| The copy shader (GLSL source and its SPIR-V, made with `glslangValidator -V --target-env vulkan1.0`) | `src/graphics/shaders/texture_load_copy_to_image.cs.glsl`, `src/graphics/shaders/vulkan_spirv/texture_load_copy_to_image_{32,64,128}bpp_cs.h` | Vulkan |
| Transient storage image descriptors | `include/rex/graphics/vulkan/command_processor.h`, `src/graphics/vulkan/command_processor.cpp` | Vulkan |

## rexglue-clear-rectangle-draws.patch

With host render targets, an EDRAM range used by one render target and then by another (a different format, MSAA mode or pitch) has its contents copied across, an ownership transfer, before the draw. Most of the transfers in this game come from its clears. Direct3D 9 on the Xbox 360 clears a render target by drawing a rectangle over it, often with 4x MSAA: there a pixel holds four samples, so the rectangle covers twice as many samples a pixel as the 2x MSAA the scene is drawn with. Each such clear made the emulator copy the whole range into a 4x MSAA host render target, draw the rectangle there, and copy it all back for the next 2x draw. At 3x, per frame, that was:
- in the city, standing at the spawn point, 3.6 ms of a 13.7 ms GPU frame (D3D12);
- in a race, 3.3 ms of 13.2 ms (D3D12).

The transfer log (`launcher/dev/edram-transfer-log.sdk.patch` and `xf-classify.py` in the fork) put 49% of the transferred samples on the clears' own targets and 28% on the copies back. The main scene is drawn in two tiles of 1280x512 and 1280x208 (2x MSAA, `2_10_10_10_FLOAT` and `D24FS8`), each cleared at 4x MSAA. Small reflection and shadow targets, and the HUD's depth, are cleared the same way.

Such a draw is now done as a clear of the host render targets, the way a resolve's clear is (`RenderTargetCache::ClearRectangleDraw`, called by both backends before processing a draw's primitives). A draw qualifies when:
- **Shape:** it is one rectangle (a 3-vertex rectangle list, auto-indexed) in screen space: no viewport transform, no clipping, culling or polygon offset, w written and above 0, with edges on pixel boundaries (with the half-pixel offset only when `half_pixel_offset` applies it to draws).
- **Depth and stencil,** if used: always passing and replacing everything, so z enabled and written with the "always" test, stencil enabled with "always" and "replace" and a full write mask, and no separate back-face state.
- **Color:** at most one color target, all of its components written, with blending off (one/zero/add).
- **Pixel shader:** none, or one that kills nothing, writes no depth, uses no texture fetches and passes no alpha test.
- **The values:**
  - The vertex shader and the pixel shader are run on the CPU with the SDK's shader interpreter, as the draw extent estimator already does for vertices, the vertex shader with its registers zeroed for each vertex, as on the host.
  - Depth is the rectangle's z, which must be the same at all three vertices. For `D24FS8` it must be exact in 20e4: drawing stores it in float32 without rounding, and the clear rounds it.
  - Color is the pixel shader's output from interpolators that are the same at all three vertices. It must not depend on any other register: the shader is run twice with those registers at 0 and at 1, and both results must match.
  - Formats handled are `8_8_8_8`, `2_10_10_10` and `2_10_10_10_FLOAT`. The first two are cleared with the guest value, as in a resolve; the host rounds and clamps to them the same way when drawing. `2_10_10_10_FLOAT` is drawn into a float16 host render target as the color times the exponent bias, not clamped to 7e3 or to its 2-bit alpha: a draw of alpha 1 with the main scene's bias of 4 leaves 16. So it is cleared with those float16 values (exact in float16), not the 32-bit guest value.
  - Depth rectangles must be multiples of 8 pixels in the mode cleared in, as storing host depth for the transfers works in 8x8 blocks.
  - Anything else is drawn as before. In the city and the race at 3x every clear rectangle that got this far was cleared, none turned back by these value and alignment checks.

The clear goes to the host render target with the MSAA mode the range was next drawn with after the same clear the previous time (learned per render target and rectangle, at most 256 of them before starting over; the draw's own mode until then), with the rectangle converted to that mode's pixels (4x pixels are 2x1 pixels of 2x MSAA and 2x2 of 1x). The tiles it covers change owner with the rectangle cut out of the transfers, as for resolve clears. So the main scene's tiles are cleared straight in their 2x MSAA targets and need no transfer at all, and the HUD's depth in its 1x one. The depth value goes through the same conversion as a resolve clear's (24-bit unorm for `D24S8`). Setting `clear_rectangle_draws` (default on; `false` draws them again).

Measured on Windows, RTX 5080 and Ryzen 7 9800X3D, with the launcher's settings, at 3x (2x MSAA). GPU timestamps (`gpu-timestamps.sdk.patch`, `gpu-timestamps-vulkan.sdk.patch`), one timedemo run a side, median frame:

| | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| City, standing: GPU frame | 13.73 ms | 10.21 ms (-25.6%) | 12.72 ms | 10.54 ms (-17.1%) |
| City: transfers and clears | 3.61 ms | 0.31 ms | 2.72 ms | 0.37 ms |
| Race: GPU frame | 13.15 ms | 10.44 ms (-20.6%) | 14.16 ms | 13.11 ms (-7.4%) |
| Race: transfers and clears | 3.30 ms | 0.35 ms | 2.61 ms | 0.42 ms |

Scripted timedemo (`launcher/dev/devrun.py`; updates of exactly 1/60 s, uncapped). `drive-spawn.pad` is fullscreen, with five runs per configuration; `race.pad` is windowed, with three. Runs alternate after a warm-up. Median frame time, with the 1% low in brackets:

| Segment | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| Free roam, standing | 13.98 ms (61 FPS) | 10.67 ms, -23.7% (74 FPS) | 12.10 ms (70 FPS) | 9.78 ms, -19.2% (82 FPS) |
| Drive from the spawn point | 13.81 ms (59 FPS) | 10.27 ms, -25.6% (74 FPS) | 12.27 ms (69 FPS) | 9.83 ms, -19.9% (84 FPS) |
| Map zoom-out (loading) | 12.69 ms (22 FPS) | 8.73 ms, -31.2% (26 FPS) | 11.33 ms (29 FPS) | 7.73 ms, -31.8% (30 FPS) |
| Race | 12.92 ms (57 FPS) | 10.54 ms, -18.4% (69 FPS) | 13.62 ms (59 FPS) | 11.86 ms, -12.9% (64 FPS) |

- **Native resolution:** no change on either API (6.82 to 7.01 ms in all three segments, within 1%), as one of the game's own threads holds the frame there.
- **D3D12 at 3x with a 60 FPS cap:** pacing is unchanged, 97.3% of frames within 1 ms of the target on against 97.1% off.

Checks:
- **Screenshots** with the setting switched on and off within one run (the dev pad's `set` command), 3 engine frames apart, standing in the city, in its pause menu, driving, in the map zoom-out and in the race (rain, midnight, opponents, HUD), on D3D12 and Vulkan. Standing and paused, on against off differs as little as two shots with the same setting (mean 0.27 against 0.14 to 0.33 out of 255); the moving scenes show nothing out of place.
- **D3D12 debug layer,** in the city run with the setting on and off and in the race with it on: no errors, and the same few creation warnings either way.
- **Vulkan validation,** in the city run with the setting on and off and in the race with it on: only `vkCmdEndQuery` 07007 and 01923, the occlusion query that this fork's other changes don't cause either.

Built and run on Windows only, with an NVIDIA RTX 5080.

| Change | Files | Affects |
|---|---|---|
| Clear rectangle draws: detection, values, MSAA mode, ownership and the clear | `include/rex/graphics/pipeline/render_target/cache.h`, `src/graphics/pipeline/render_target/cache.cpp` | D3D12, Vulkan |
| The transfers and the clear, as in a resolve | `include/rex/graphics/d3d12/render_target_cache.h`, `include/rex/graphics/vulkan/render_target_cache.h` | D3D12, Vulkan |
| The call before a draw's primitives are processed | `src/graphics/d3d12/command_processor.cpp`, `src/graphics/vulkan/command_processor.cpp` | D3D12, Vulkan |

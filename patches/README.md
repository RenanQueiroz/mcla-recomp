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
```

`scripts\make_pc.ps1` and `ps5/make_ps5.sh` apply them themselves, each once, telling one that is in by a name it adds, and they record each patch's SHA-256 in the SDK checkout's `.mcla-patches`. When a patch that is in has changed since it was applied, or has no record (an SDK patched by hand or by an older script), they stop rather than build with an older version of it: patch the SDK afresh (`launcher\build-game.ps1 -ResetSdk`, which the launcher does by itself after an update that changes a patch, or delete the SDK checkout and run the script again).

On Linux, two more go on top, from the same folder:

```
git apply ../../patches/rexglue-linux-procmap-query.patch
git apply ../../patches/rexglue-linux-wait-sleep.patch
```

They make page-protection lookups and multi-handle waits cheap on Linux. The Windows build needs neither. The PS5 build also applies `rexglue-linux-wait-sleep.patch` (`ps5/make_ps5.sh` does it): the PS5 runs the same POSIX threading code, where a wait shorter than 1 ms spun instead of sleeping.

The first was several files at first; they are combined now because several touch the same build files and would not apply independently. The second goes on top of the first (both change `src/graphics/vulkan/command_processor.cpp`). The build scripts apply all of them, and add the second and the ones after the FFmpeg patch to an SDK checkout that was patched before they existed. Those later patches go on in the order listed, as several change neighbouring lines.

## rexglue-v0.10.0-mcla.patch

| Change | Files | Affects |
|---|---|---|
| Vblank timer resync: the guest tick count can step backwards, which made an unsigned difference wrap and fire vblank interrupts without end | `src/graphics/graphics_system.cpp` | all platforms |
| Per-frame counters kept in Release builds, and their CSV path taken from `REX_PERF_LOG_CSV` (the GPU plugin has its own copy of the logger state, so a path set by the host never reached it) | `CMakeLists.txt`, `src/core/CMakeLists.txt`, `src/system/CMakeLists.txt`, `src/core/perf/counter.cpp` | all platforms |
| Debug-only register lookup removed from every GPU register write in Release builds | `src/graphics/command_processor.cpp` | all platforms |
| PS5 platform: everything else. Platform detection, libc and libc++ differences, anonymous shared memory with a shared-object address reservation at a kernel-chosen base, 16 KiB host pages, signals without realtime signals, no X11/Wayland/RenderDoc, static runtime and GPU libraries, no desktop install rules | the remaining files | PS5 only, behind `REX_PLATFORM_PS5` or `if(PS5)` |

## rexglue-minmax-blend-factor.patch

The car reflection fix: cars came out white with a glow around them, worst at night. The Xbox 360's GPU applies the source blend factor in the MIN and MAX blend equations; Direct3D 12 and Vulkan ignore blend factors there. The game draws the light glows in the cars' environment map with `max(source x SrcAlpha, destination)`, so every glow became a solid square, and the paint mirrored those. With the patch the translated pixel shader multiplies by its own alpha in that case.

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

Standing in free roam the frame rate is held by one of the game's own threads (busy 96 to 98%), and at 3x by the GPU, so the thread shows only where the GPU command thread limits; there it took the GPU command thread from 86% to 60% busy (the submission thread 36%; measured before upload hoisting). Built and run on Windows only; upstream's PS5 notes measured about 1 ms per blocking submit while driving and 2 to 3 ms in a frame-rate dip, which this moves off the GPU command thread.

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

## rexglue-texture-load-direct.patch

With resolution scaling, render targets the game resolves to memory go to the scaled resolve buffer, and every texture over that memory is decoded from it again at its next use. At 3x the screen-sized ones are reloaded every frame, some several times: this game reuses memory, so the 3840x2160 `8_8_8_8` texture at one address is loaded three times a frame and the scene's depth twice. Each load was two passes over 33 to 66 MB, the load shader decoding into a scratch buffer and then a copy of that into the texture (`CopyTextureRegion` on D3D12, the copy of `rexglue-vulkan-texture-load-storage-copy.patch` or `vkCmdCopyBufferToImage` on Vulkan), and the copy took half the time. Texture loads were 1.62 ms of a GPU frame of about 11 ms on D3D12 and 1.85 ms on Vulkan, standing in the city at 3x.

Such a texture is now decoded straight into itself through an unsigned integer view of the texel size, in one pass. Setting `texture_load_direct` (default on; `false` loads through the buffer again at once, while turning it back on takes effect on restart, as the pipelines are created at startup only). It applies to textures that are:
- resolved data with resolution scaling, with one mip level, 2D and one layer, and not so small (16 texels or less) that the base is in the packed mip tail, which is loaded as a whole through the buffer;
- loaded by the 32bpb, 64bpb or either depth load shader (unchanged formats, 20e4 depth and 24-bit unorm depth);
- on D3D12, in a format with an allowed unsigned view: `R32_UINT` for the typeless 32-bit formats `R8G8B8A8`, `B8G8R8A8`, `R10G10B10A2`, `R16G16` and `R32` (the one cast to a format with other components that D3D12 allows for UAVs), `R16G16B16A16_UINT` for `R16G16B16A16`. Such a texture is created typeless (`R32_FLOAT` and `R16G16B16A16_FLOAT` become `R32_TYPELESS` and `R16G16B16A16_TYPELESS`; the views are unchanged) with unordered access;
- on Vulkan, those with the storage view of `vulkan_texture_load_storage_copy` (`r32ui` or `rgba16ui`), so on NVIDIA only by default like that.

The new shaders are the load shaders with the stores going to the texture instead of the buffer, the guest addressing, endian swap and conversion included the same way (`src/graphics/shaders/texture_load_direct_scaled.xesli` and four `.cs.xesl` files). The SDK's load shaders are built from xenia-project/xenia's shader sources at 0b2ffa3 (that commit's `texture_load_32bpb_scaled.cs.xesl` compiles to the SDK's bytecode exactly), so these are built against those sources too: with that commit's `src/xenia/gpu/shaders` and `src/xenia/ui/shaders`, fxc (`/T cs_5_1 /O3`, as xenia-canary's `tools/build/compile_shader_dxbc.py`) and glslangValidator with spirv-opt (`compile_shader_spirv.py`).

Measured on Windows, RTX 5080 and Ryzen 7 9800X3D, with the launcher's settings, at 3x (2x MSAA).
GPU timestamps (`gpu-timestamps.sdk.patch`, `gpu-timestamps-vulkan.sdk.patch`, `texload-split-timestamps.sdk.patch`), one timedemo run a side, texture loads per frame (median):

| At 3x | D3D12, before | D3D12, after | Vulkan, before | Vulkan, after |
|---|---|---|---|---|
| Standing in the city | 1.62 ms | 1.12 ms | 1.85 ms | 1.36 ms |
| Race | 1.25 ms | 0.94 ms | 1.50 ms | 1.24 ms |

Scripted timedemo (`launcher/dev/devrun.py`; updates of exactly 1/60 s, uncapped). `drive-spawn.pad` is fullscreen with five runs per configuration, `race.pad` windowed with three; runs alternate after a warm-up. Median frame time at 3x, with the 1% low in brackets:

| Segment | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| Free roam, standing | 10.58 ms (73 FPS) | 10.15 ms, -4.1% (77 FPS) | 9.83 ms (87 FPS) | 9.40 ms, -4.4% (88 FPS) |
| Drive from the spawn point | 10.22 ms (73 FPS) | 9.69 ms, -5.2% (74 FPS) | 10.00 ms (81 FPS) | 9.41 ms, -5.9% (87 FPS) |
| Map zoom-out (loading) | 9.67 ms (25 FPS) | 8.12 ms, -16% (27 FPS) | 8.75 ms (32 FPS) | 7.86 ms, -10% (32 FPS) |
| Race | 10.41 ms (69 FPS) | 9.93 ms, -4.6% (72 FPS) | 11.86 ms (62 FPS) | 11.64 ms, -1.9% (62 FPS) |

- **The map zoom-out** varies 14 to 20% between runs, the race about 6% (`race-benchmark-2026-10-15.md`); free roam and the drive 2 to 3.5%.
- **Native resolution:** nothing changes, as nothing there is resolution-scaled (6.80 to 6.94 ms in every segment, on and off, both APIs).
- **D3D12 at 3x with a 60 FPS cap:** pacing unchanged, 96.6% of frames within 1 ms of the target on against 96.8% off.

Checks:

- **Screenshots** with the setting switched on and off within one run (the dev pad's `set`), 3 engine frames apart: standing in the city, in its pause menu, driving, in the map zoom-out and in the race (rain, opponents, HUD), on both APIs. Standing and paused, on against off differs no more than two shots with the same setting (mean 0.23 to 0.35 against 0.25 to 0.40 out of 255); the moving scenes show nothing out of place: shadows, reflections, the wet road, smoke, motion blur, the HUD and the minimap.
- **D3D12 debug layer** in that city run, with the setting switched on and off: no errors, the same few creation warnings as without the change. The `R32_UINT` and `R16G16B16A16_UINT` views of the typeless textures are accepted.
- **Vulkan validation** in that run: only `vkCmdEndQuery` 07007 and 01923. With synchronization validation, only the write after write between load dispatches into the scratch buffer that was there before; nothing on the images.
- **The attract movie** at 3x, D3D12 and Vulkan: every capture while it plays (221 and 222, every 150 ms) matches a frame of the decoded movie, with the largest difference 0.14, as in the earlier checks.

Built and run on Windows only, with an NVIDIA RTX 5080.

| Change | Files | Affects |
|---|---|---|
| The setting, which load shaders and textures | `include/rex/graphics/flags.h`, `include/rex/graphics/pipeline/texture/cache.h`, `src/graphics/pipeline/texture/cache.cpp` | D3D12, Vulkan |
| Typeless textures with unordered access, the view, the pipelines | `include/rex/graphics/d3d12/texture_cache.h`, `src/graphics/d3d12/texture_cache.cpp` | D3D12 |
| The pipelines and the storage image descriptor | `include/rex/graphics/vulkan/texture_cache.h`, `src/graphics/vulkan/texture_cache.cpp` | Vulkan |
| The shaders (XeSL sources, DXBC and SPIR-V) | `src/graphics/shaders/texture_load_*direct_scaled*`, `src/graphics/shaders/bytecode/d3d12_5_1/texture_load_*_direct_scaled_cs.h`, `src/graphics/shaders/vulkan_spirv/texture_load_*_direct_scaled_cs.h` | D3D12, Vulkan |

## rexglue-async-pipelines-no-frame-wait.patch

**What happened before.** With `async_shader_compilation` (on by default), a draw whose pipeline is new is left out while the pipeline is created on the pipeline creation threads. Then, at the end of every submission, `PipelineCache::EndSubmission` on both APIs:
- created the rest of the queue on the GPU command thread;
- waited for every creation still in progress.

So the frame was held for as long as the slowest compile, and still went out without those draws. On Vulkan, each new pipeline also got a placeholder first: the real vertex shader with a stand-in pixel shader, created on the GPU command thread. Draws with a placeholder are left out anyway. Creating it compiles the vertex shader, which takes about as long as the real pipeline when the driver hasn't cached it.

**When it matters.** With the shader storage and the driver's own cache warm, nothing is created during play. A first session, new places or races, a driver update or a change to the translated shaders all bring compiles. Measured on NVIDIA with the driver's cache cold:
- D3D12 pipelines took up to 80 to 100 ms each;
- Vulkan pipelines took 13 to 18 ms at the median and up to 465 ms.

**The change:**
- **No wait at the end of a submission.** With async compilation, the end of a submission neither waits for pending pipelines nor creates them. Nothing recorded refers to them: a draw whose pipeline is pending is left out before anything is recorded for it, and on Vulkan a placeholder is never bound. xenia-canary's D3D12 `PipelineCache::EndSubmission` stopped waiting the same way. `async_pipeline_end_of_frame_wait` (default false) brings the wait back.
- **A short wait at the draw.** A draw whose pipeline is pending waits for it within a budget per frame, `async_pipeline_wait_budget_ms`:
  - default 4; 0 leaves the draw out at once; -1 waits as long as it takes;
  - the wait is a condition variable notified by the creation threads;
  - pipelines the driver has cached (well under a millisecond) are drawn in the same frame;
  - D3D12 takes the root signature of a pipeline created asynchronously again at every draw, as the creation thread stores it with the pipeline, possibly after `ConfigurePipeline` returned. The bindful root signatures, which the creation threads create too, are behind a lock;
  - a draw inside an occlusion query isn't left out, which the query would count as hidden: a new pipeline for it is created right away, on the GPU command thread, and one that an earlier draw queued is waited for (up to half a second, outside the budget). Waiting for a new one instead held a cold title screen's frame for 400 ms, as its pipeline waited behind the others in the queue.
- **Vulkan: no placeholders.** No placeholder pipelines are created (`vulkan_async_placeholder_pipelines`, default false; true creates them as before).
  - Without them, depth-only pipelines are created asynchronously too, unless the vertex shader exports to memory (memexport), which the guest may read.
  - A pipeline whose creation fails stays left out and is no longer waited for. It doesn't make the frame incomplete either, which would otherwise hold every later frame back from being presented.
  - A pipeline is published as its layout, then the pipeline, then `is_placeholder` cleared (release), and read in the opposite order (acquire). D3D12 already stores a pipeline's root signature before the pipeline.
  - D3D12 still creates depth-only pipelines on the GPU command thread. Its async path translates the shaders on the creation thread and doesn't handle a missing pixel shader. They cost 70 to 200 ms in a whole cold session there.
- **Vulkan: a limit on skipped presents.** `vulkan_async_skip_incomplete_frames` (on) skips presenting a frame that left a draw out. Without the end-of-submission wait, that could hold the picture for a whole compile. `vulkan_async_skip_incomplete_frames_max_ms` (default 100; 0 for no limit) shows the incomplete frames after that long.

**The calls that end a submission in the middle of a frame:**
- the primary-buffer end and the threaded-submission split asked `CanEndSubmissionImmediately` for no pipelines being created. Without the end-of-submission wait, pending pipelines no longer hold them back;
- the readbacks (`CheckSubmissionFence`, memexport and resolve readback, the occlusion queries) need the GPU to have run what was recorded, which never refers to a pending pipeline.

**Measurements.** Measured on Windows, RTX 5080 and Ryzen 7 9800X3D, with the launcher's settings.
- **Method:**
  - `race.pad` under `--mcla_fixed_dt=60`: the title, the map zoom-out, the menus, the race's intro and 90 s of the race.
  - "Cold" is an empty storage with the driver's cache missed on purpose (the dev-only `tmp_shader_salt` of `launcher/dev/shader-stutter-timers.sdk.patch`).
  - Two runs each; frames from the title on.
  - "Before" is `--async_pipeline_end_of_frame_wait=true --async_pipeline_wait_budget_ms=0 --vulkan_async_skip_incomplete_frames_max_ms=0 --vulkan_async_placeholder_pipelines=true`.

| Cold | Frames over 50 ms | Over 100 ms | Worst frame | Time in frames over 50 ms | Map zoom-out 1% low | Race 1% low |
|---|---|---|---|---|---|---|
| D3D12, 1x, before | 7.0 | 1.5 | 326 ms | 0.76 s | 18.6 FPS | 73.9 FPS |
| D3D12, 1x, after | 3.5 | 2.0 | 159 ms | 0.38 s | 26.1 FPS | 79.9 FPS |
| D3D12, 3x, before | 10.0 | 2.0 | 398 ms | 1.02 s | 13.4 FPS | 56.7 FPS |
| D3D12, 3x, after | 5.0 | 2.0 | 190 ms | 0.52 s | 23.0 FPS | 64.8 FPS |
| Vulkan, 1x, before | 83.0 | 57.5 | 1,850 ms | 17.35 s | 2.5 FPS | 13.4 FPS |
| Vulkan, 1x, after | 3.0 | 2.0 | 173 ms | 0.37 s | 19.4 FPS | 55.1 FPS |
| Vulkan, 3x, before | 89.0 | 57.5 | 1,919 ms | 17.56 s | 2.4 FPS | 15.2 FPS |
| Vulkan, 3x, after | 3.5 | 1.5 | 246 ms | 0.43 s | 22.1 FPS | 50.8 FPS |

- **Frames still over 100 ms:** one at the title screen (100 to 185 ms in every run, warm ones included, with nothing created) and the race's loading.
- **Time the frame end waited per session:**
  - before: 1.3 to 1.5 s on D3D12; 11.7 to 12.7 s on Vulkan, plus 4.3 to 4.5 s of placeholders;
  - after: none.
- **What is left on the GPU command thread:**
  - D3D12's depth-only pipelines;
  - Vulkan's shader translations, about 420 ms a session, up to 136 ms in a frame.
- **With the storage covering the run:** nothing is created during play, so nothing changes. That is the case for a warm run, and for one after a driver update, whose compiles all happen in the preload at start-up.

**What shows.** The draws left out until their pipelines are ready:
- **D3D12:** 15,000 to 17,500 draws over 130 to 145 frames in a cold session, against 13,000 to 16,000 over 190 to 230 before, when those frames were held as well.
- **Vulkan:** 220,000 to 490,000 draws over 530 to 620 frames; 205 to 250 presents skipped.
- **Screenshots:** one shot of a cold Vulkan zoom-out has buildings missing for that moment, where before the zoom-out froze for up to 1.9 s and fell 1 to 2 s behind.

Checks:

- **D3D12 debug layer**, cold 3x race runs: no errors. Besides the creation warnings that were there before (and the list of live objects at exit), each cold run has 3 to 9 `RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS` warnings on render targets. Those are a render target's transitions queued for a draw that was then left out, followed by the next draw's, in one barrier call. Draws were left out the same way before, and the warning is the same with `async_pipeline_end_of_frame_wait`; warm runs, which leave nothing out, have none.
- **Vulkan validation**, cold 3x race runs: only `ShaderOutputNotConsumed` and `vkCmdEndQuery` 01923 and 07007, all there before.
- **After the review fixes** (occlusion queries, failed creations, the order of a pipeline's publication, submissions ending while pipelines are created), one cold 3x race run each:
  - D3D12: 4 frames over 50 ms, 1 over 100 ms (the title screen's, 281 ms), 0.51 s in frames over 50 ms, map zoom-out 1% low 26.2 FPS, race 67.7 FPS;
  - Vulkan: 3, 1 (224 ms), 0.38 s, 24.4 FPS, 53.2 FPS.
  - Warm 3x drive, two runs a side: D3D12 9.73 and 9.81 ms against 9.64 and 9.60 ms before; Vulkan 9.22 and 9.44 against 9.42 and 9.55 ms. One warm Vulkan run had a 283 ms frame in the drive with no pipeline work at all.
- **Warm benchmark** (`drive-spawn.pad`, timedemo, fullscreen, five runs a side alternating after a warm-up): the map zoom-out, free roam and the drive within 1.3% of before at native and 3x on both APIs; nothing is created there.
- **60 FPS cap**, native: 98.3 to 99.3% of frames within 1 ms of the target, against 98.5 to 100% before.

Measurements and method: `launcher/dev/shader-stutter-2026-10-15.md` in the fork. Built and run on Windows only, with an NVIDIA RTX 5080.

| Change | Files | Affects |
|---|---|---|
| The settings | `include/rex/graphics/flags.h`, `src/graphics/command_processor.cpp`, `src/graphics/vulkan/command_processor.cpp` | D3D12, Vulkan |
| No wait at the end of a submission; the wait for one pipeline | `include/rex/graphics/d3d12/pipeline_cache.h`, `src/graphics/d3d12/pipeline_cache.cpp`, `include/rex/graphics/vulkan/pipeline_cache.h`, `src/graphics/vulkan/pipeline_cache.cpp` | D3D12, Vulkan |
| The budget per frame, at the draw | `include/rex/graphics/d3d12/command_processor.h`, `src/graphics/d3d12/command_processor.cpp`, `include/rex/graphics/vulkan/command_processor.h`, `src/graphics/vulkan/command_processor.cpp` | D3D12, Vulkan |
| No placeholders; depth-only pipelines asynchronous; the limit on skipped presents | `include/rex/graphics/vulkan/pipeline_cache.h`, `src/graphics/vulkan/pipeline_cache.cpp`, `src/graphics/vulkan/command_processor.cpp` | Vulkan |

## rexglue-vulkan-pipeline-storage-parallel-translation.patch

When the shader storage is loaded, Vulkan translated the shaders of the stored pipelines one by one on the GPU command thread before creating any pipeline. That was 740 translations, 300 to 310 ms of every Vulkan start. D3D12's preload is about 50 ms in all.

They are now translated on as many threads as pipeline creation uses (`vulkan_pipeline_storage_translation_threads`, default -1; 1 translates them one by one as before):
- all of one shader on the same thread, since its first translation also gives it its bindings;
- the shared binding layouts under the cache's `layouts_mutex_`;
- each thread with a translator of its own;
- no more threads than logical processors;
- if a thread can't be created, the calling thread translates what the others don't take.

Translation: 303 to 311 ms before, 49 to 51 ms after (12 threads), in every run, warm or not. Creating the stored pipelines was already parallel: 21 to 25 ms with the driver's cache warm, 5.0 s once after a change to the translated shaders. Built and run on Windows only, with an NVIDIA RTX 5080.

| Change | Files | Affects |
|---|---|---|
| The setting, the parallel translation | `src/graphics/vulkan/pipeline_cache.cpp` | Vulkan |

## rexglue-vulkan-pipeline-cache-file.patch

Keeps the driver's pipeline cache (`VkPipelineCache`) between runs, after xenia-canary's `VulkanPipelineCache`. It is in `<cache root>/shaders/local/<title ID>.vk.bin` and:
- is loaded before the stored pipelines are created. The whole file must be read, and its header (`VkPipelineCacheHeaderVersionOne`: length, version, vendor and device ID, `pipelineCacheUUID`) must match the device, or the cache starts empty;
- is passed to every pipeline creation;
- is saved by the storage writing thread every 10 s while new pipelines are being created, and when the storage is shut down. It is written next to the file, flushed to the disk and renamed;
- is not written again while the cache's size is what was loaded or last saved, as in a run that only creates pipelines already in it;
- stays marked for saving until a save succeeds.

`vkCreatePipelineCache`, `vkDestroyPipelineCache` and `vkGetPipelineCacheData` are added to the device's function table.

Off by default (`vulkan_pipeline_cache_file`): on NVIDIA's Windows driver, which keeps a disk cache of its own, it gained nothing.
- **Cold start:** with the driver's cache cold, the preload of 1,030 pipelines took 5.0 s with the file and without.
- **Warm start:** with both warm, 21 against 25 ms.
- **Pipelines missing from the file:** in a run whose pipelines the driver had cached but the file didn't yet, creating them took about ten times as long (median 1.5 against 0.16 ms).
- **Size:** the file is 19 MB for 1,030 pipelines.

It is meant for drivers without a disk cache of their own, possibly the PS5's, where it is worth trying. Not measured there. Built and run on Windows only, with an NVIDIA RTX 5080.

| Change | Files | Affects |
|---|---|---|
| The setting, loading, saving | `include/rex/graphics/vulkan/pipeline_cache.h`, `src/graphics/vulkan/pipeline_cache.cpp` | Vulkan |
| The three functions | `include/rex/ui/vulkan/functions/device_1_0.inc` | Vulkan |

## rexglue-resolve-4xmsaa-depth-samples.patch

The EDRAM buffer path's `resolve_fast_32bpp_4xmsaa` read some wrong samples of 4x MSAA depth. A thread copies 8 pixels and loads the 16 consecutive values that start at the first pixel's sample, but depth tiles have their left and right halves swapped in the EDRAM layout. Half a tile is 20 pixels times the resolution scale, so at 1x, 3x and the other odd scales the boundary falls between pixels 3 and 4 of one thread in every row of a tile, and the values that thread loaded for pixels 4 to 7 came from right after pixel 3 in the other half: the next row of samples (the pixel's other vertical sample) or, from a tile's last row, the next tile. In this game that's the four 640x640 4x depth resolves a frame in daylight: some texels had a neighbouring sample's depth, which showed on screen in columns 4 pixels wide, 120 pixels apart at 3x, by 1 to 169 levels out of 255 at edges.

Pixels 4 to 7 now have their own address. In xenia-project/xenia's source at 0b2ffa3 (`src/xenia/gpu/shaders/resolve_fast_32bpp_4xmsaa.xesli`):

```diff
@@ -34,12 +34,25 @@
     return;
   }
   uint2_xe pixel_index = in_global_thread_id_xe.xy << uint2_xe(3u, 0u);
+  uint2_xe source_pixel_index =
+      uint2_xe(pixel_index.x,
+               max(pixel_index.y,
+                   resolve_info.half_pixel_offset_fill_source.y)) +
+      resolve_info.edram_offset_scaled;
   uint source_address_int4s =
       XeEdramOffsetInts(
-          uint2_xe(pixel_index.x,
-                   max(pixel_index.y,
-                       resolve_info.half_pixel_offset_fill_source.y)) +
-              resolve_info.edram_offset_scaled,
+          source_pixel_index,
+          resolve_info.edram_base_tiles, true, resolve_info.edram_pitch_tiles,
+          kXenosMsaaSamples_4X, resolve_info.edram_is_depth, 0u,
+          XeResolveFirstSampleIndex(resolve_info.sample_select),
+          resolve_info.resolution_scale)
+      >> 2u;
+  // The halves of depth tiles are swapped in the EDRAM. A half is 20 pixels
+  // times the resolution scale, so with an odd scale, pixels 4...7 of a thread
+  // may be in the other half than 0...3, not right after them.
+  uint source_address_int4s_4567 =
+      XeEdramOffsetInts(
+          source_pixel_index + uint2_xe(4u, 0u),
           resolve_info.edram_base_tiles, true, resolve_info.edram_pitch_tiles,
           kXenosMsaaSamples_4X, resolve_info.edram_is_depth, 0u,
           XeResolveFirstSampleIndex(resolve_info.sample_select),
@@ -54,18 +67,18 @@
     pixels_0123.zw = array_buffer_load_xe(
         xe_resolve_edram, source_address_int4s + 1u).xz;
     pixels_4567.xy = array_buffer_load_xe(
-        xe_resolve_edram, source_address_int4s + 2u).xz;
+        xe_resolve_edram, source_address_int4s_4567).xz;
     pixels_4567.zw = array_buffer_load_xe(
-        xe_resolve_edram, source_address_int4s + 3u).xz;
+        xe_resolve_edram, source_address_int4s_4567 + 1u).xz;
   } else {
     pixels_0123.xy = array_buffer_load_xe(
         xe_resolve_edram, source_address_int4s).yw;
     pixels_0123.zw = array_buffer_load_xe(
         xe_resolve_edram, source_address_int4s + 1u).yw;
     pixels_4567.xy = array_buffer_load_xe(
-        xe_resolve_edram, source_address_int4s + 2u).yw;
+        xe_resolve_edram, source_address_int4s_4567).yw;
     pixels_4567.zw = array_buffer_load_xe(
-        xe_resolve_edram, source_address_int4s + 3u).yw;
+        xe_resolve_edram, source_address_int4s_4567 + 1u).yw;
   }
   dont_flatten_xe
   if (pixel_index.x == 0u &&
```

The SDK has no sources for its resolve shaders, so the patch is the rebuilt headers of the two variants (unscaled and scaled) for both APIs. D3D12: fxc's `/Fh` output with its default flags, clang-formatted, which rebuilds the SDK's current headers of these two shaders byte for byte from the unchanged source. Vulkan: glslangValidator, `spirv-opt -O -O --canonicalize-ids --strip-debug` and spirv-dis in the layout of `compile_shader_spirv.py`, clang-formatted, which does the same. Colour resolves are unaffected: their halves aren't swapped, so pixel 4's address is where it was.

Measured on Windows, RTX 5080 and Ryzen 7 9800X3D, with the launcher's settings: with `rexglue-direct-resolve.patch`'s validation build, every direct resolve compared byte for byte with this path matched, the 4x depth ones included, at 1x, 2x and 3x on both APIs (162,550 resolves, of which 9,942 4x depth). Before the fix, 255 of the 263 4x depth resolves compared in the city at 3x on D3D12 differed (`launcher/dev/direct-resolve-2026-10-15.md` in the fork); at 2x the boundary never falls inside a thread. The 640x640 4x depth copies take as long as before at 3x (25 to 26 us each on D3D12, 29 to 30 us on Vulkan, against 25 to 27).

Built and run on Windows only, with an NVIDIA RTX 5080.

| Change | Files | Affects |
|---|---|---|
| The shader, unscaled and scaled | `src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_cs.h`, `src/graphics/shaders/bytecode/d3d12_5_1/resolve_fast_32bpp_4xmsaa_scaled_cs.h`, `src/graphics/shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_cs.h`, `src/graphics/shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_scaled_cs.h` | D3D12, Vulkan |

## rexglue-direct-resolve.patch

**What happened before.** With host render targets, every resolve first dumped the render targets over its area into the EDRAM buffer, every sample of every pixel (`DumpRenderTargets`), and then the resolve shader converted that into the destination. The SDK has a setting for doing it directly, `direct_host_resolve` (on by default), but it was a stub that dumped all the same (`TryResolveCopyDirectly`). At 3x the dumps were 60% of the resolve time: 1.35 ms of the GPU frame standing in the city by day on D3D12 (four 640x640 4x MSAA depth quadrants, the scene's depth and colour, each resolved twice), 1.0 ms in the race.

**What it does now.** A resolve copy reads the host render target that owns its area directly. The new shaders (`src/graphics/shaders/resolve_direct*.xesl(i)`) are the resolve copy shaders with every EDRAM buffer load replaced by a load of the same sample from the render target, converted to its EDRAM representation exactly as dumping does (`GetOrCreateDumpPipeline`, `GetDumpPipeline`): 8_8_8_8 and 2_10_10_10 from their UNORM views, the piecewise-linear gamma, 7e3 from float16 (D3D12 `max`/`min`, Vulkan `NClamp` as in the dumping), the 16- and 32-bit formats from their unsigned integer views, 24-bit unorm and 20e4 depth (with each API's rounding setting) and stencil. Everything after the loads (sample selection and averaging, exponent bias, packing, swaps, tiling, endian) is the original code. A sample of the resolve's area in the EDRAM sample grid, offset by whole tiles to the render target's base, is the same sample in the render target's texture; then the pixel and sample are as in the dumping (with 2x MSAA emulated with 4x too).

It is used when, otherwise the resolve dumps as before:
- there's a direct shader for the copy shader: fast 32bpp, fast 64bpp, full 8bpp, full 16bpp, full 32bpp or full 64bpp (the ones this game uses; full 128bpp goes through the EDRAM buffer), at any resolution scale, from single-sampled or multisampled render targets. The fast 4x MSAA copy shaders use the 1x/2x MSAA ones' direct pipelines, which read any sample of a multisampled render target, so 12 pipelines are created a backend. The resolve's sample count picks the pipeline before anything else is looked up;
- one render target owns the whole area the copy reads, with the resolve's pitch, sample count and sample size, and EDRAM addressing doesn't wrap around in it;
- the render target's format has a conversion: on Vulkan, the 16- and 32-bit formats only through their unsigned integer transfer views, not the floating-point fallbacks.

`--direct_host_resolve=false` goes back to dumping at once. The 24 shaders (six copy shaders, single-sampled and multisampled sources, unscaled and scaled) are built for both APIs from the xenia-project/xenia 0b2ffa3 shader sources (whose resolve shaders compile to the SDK's resolve shader code exactly) with fxc and glslangValidator with spirv-opt, as `rexglue-texture-load-direct.patch`'s; the headers are 32-bit words without the disassembly (3.1 MB for the 48). The per-sample loads and the sample averaging are loops that aren't unrolled, so the conversions and unpacking are there once: the full shaders are about 20 KB of DXBC, half of the copy shaders they replace.

- **D3D12:** a root signature like the resolve copy one, with the two source constants and two SRV tables (separate, as bindless one-use descriptors aren't contiguous): t0 the float view (colour or depth), t1 the unsigned one (colour or stencil), a null SRV for the unused one. The render target goes to `NON_PIXEL_SHADER_RESOURCE` as for dumping.
- **Vulkan:** a pipeline layout with the two sampled images in set 0 and the destination in set 1. Depth render targets use their transfer descriptor set (depth, stencil); colour ones get a two-image set when first resolved directly (the render target's view and a 1x1 dummy image of the other kind), freed with the render target. A multisampled colour source needs multisampled integer sampled images (for itself or for the dummy), which Vulkan only guarantees single-sampled: without them, those go through the EDRAM buffer, while multisampled depth and every single-sampled source still resolve directly. A warning at startup says which sources fall back. The dummies' first barrier skips any that couldn't be created.

**With `rexglue-resolve-4xmsaa-depth-samples.patch`.** The EDRAM buffer path read some wrong samples of 4x MSAA depth at odd resolution scales, 1x included; the direct path reads every sample where it is. That patch, before this one, fixes the EDRAM path, so both paths give the same result whichever one a resolve takes.

Measured on Windows, RTX 5080 and Ryzen 7 9800X3D, with the launcher's settings (2x MSAA).

GPU timestamps (`gpu-timestamps.sdk.patch`, `gpu-timestamps-vulkan.sdk.patch`, `texload-split-timestamps.sdk.patch`, `resolve-split-timestamps.sdk.patch`), one timedemo run a side, resolve time per frame (median):

| Resolves per frame | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| Native, standing in the city by day | 0.48 ms (dump 0.22) | 0.33 ms | 0.50 ms (dump 0.21) | 0.42 ms |
| Native, race | 0.54 ms (dump 0.21) | 0.38 ms | 0.55 ms (dump 0.21) | 0.48 ms |
| 2x, standing in the city by day | 1.09 ms (dump 0.58) | 0.67 ms | 0.93 ms (dump 0.49) | 0.65 ms |
| 2x, race | 1.00 ms (dump 0.49) | 0.66 ms | 0.88 ms (dump 0.41) | 0.70 ms |
| 3x, standing in the city by day | 2.23 ms (dump 1.36) | 1.08 ms | 1.90 ms (dump 1.09) | 1.09 ms |
| 3x, race | 1.79 ms (dump 1.04) | 0.93 ms | 1.65 ms (dump 0.82) | 1.10 ms |

At 3x the GPU frame standing in the city went from 10.59 to 9.04 ms on D3D12 and from 10.02 to 9.54 ms on Vulkan in these runs (10.53 to 9.41 and 10.16 to 9.23 in the first ones). At native and 2x the frames here wait for the game thread, so the time between a frame's first and last timestamps isn't the GPU's work; the resolve time saved is what a GPU-bound system gains.

Scripted runs (`launcher/dev/devrun.py`), alternating after a warm-up, median frame time with the 1% low in brackets; the timedemo has updates of exactly 1/60 s, uncapped (`drive-spawn.pad` fullscreen, `race.pad` windowed). At 3x, before full 16bpp and the unscaled shaders, five runs a configuration (three for the race):

| Segment | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| Free roam, standing (timedemo) | 10.09 ms (81 FPS) | 8.99 ms, -10.9% (91 FPS) | 9.33 ms (90 FPS) | 8.46 ms, -9.3% (99 FPS) |
| Drive from the spawn point (timedemo) | 9.79 ms (79 FPS) | 8.63 ms, -11.8% (87 FPS) | 9.39 ms (88 FPS) | 8.67 ms, -7.7% (95 FPS) |
| Map zoom-out (timedemo) | 9.04 ms (26 FPS) | 7.23 ms (31 FPS) | 8.37 ms (31 FPS) | 7.34 ms (32 FPS) |
| Free roam, real time | 10.01 ms (84 FPS) | 8.81 ms, -12.0% (90 FPS) | 9.22 ms (96 FPS) | 8.57 ms, -7.1% (102 FPS) |
| Drive, real time | 9.12 ms (89 FPS) | 8.04 ms, -11.8% (101 FPS) | 8.90 ms (96 FPS) | 8.22 ms, -7.6% (103 FPS) |
| Race (timedemo) | 10.18 ms (71 FPS) | 9.61 ms, -5.6% (75 FPS) | 11.60 ms (65 FPS) | 11.07 ms, -4.6% (67 FPS) |

With them, timedemo, three runs a configuration at 3x and for the races, five at 2x:

| Segment | D3D12, off | D3D12, on | Vulkan, off | Vulkan, on |
|---|---|---|---|---|
| 3x, free roam, standing | 10.35 ms (75 FPS) | 9.19 ms, -11.2% (85 FPS) | 9.76 ms (76 FPS) | 8.96 ms, -8.2% (86 FPS) |
| 3x, drive | 10.09 ms (74 FPS) | 8.88 ms, -12.0% (79 FPS) | 9.94 ms (69 FPS) | 9.03 ms, -9.2% (73 FPS) |
| 3x, race | 10.17 ms (71 FPS) | 9.29 ms, -8.7% (75 FPS) | 11.29 ms (66 FPS) | 10.79 ms, -4.4% (68 FPS) |
| 2x, free roam, standing | 7.45 ms (101 FPS) | 7.01 ms, -5.9% (107 FPS) | 6.89 ms (107 FPS) | 6.84 ms, -0.7% (111 FPS) |
| 2x, drive | 7.21 ms (92 FPS) | 7.12 ms, -1.2% (102 FPS) | 7.14 ms (88 FPS) | 7.05 ms, -1.3% (93 FPS) |
| 2x, race | 8.10 ms (85 FPS) | 7.86 ms, -3.0% (84 FPS) | 8.93 ms (66 FPS) | 8.29 ms, -7.2% (67 FPS) |

- **Spread between runs:** free roam and the drive 0.1 to 5.2%, the map zoom-out 0.4 to 21%, the races 1.0 to 7.8%. In the second 3x series free roam and the drive ran 2 to 6% slower than in the first, on and off alike, with the same gains (D3D12 -11 to -12% in both; Vulkan -8.2 and -9.2% against -9.3 and -7.7%); full 16bpp took the D3D12 race from -5.6% to -8.7%.
- **2x** is close to what the game thread allows here (6.8 to 6.9 ms at native), so the frame gains less than the resolve time saved; the 1% lows rise most (D3D12 drive 92 to 102 FPS).
- **With a 120 FPS cap at 3x,** frames within 1 ms of the target: D3D12 26% off, 93% on; Vulkan 84% off, 97% on. At 2x: 98.5 to 99.5% either way.
- **With a 60 FPS cap at 3x:** unchanged, 97 to 99% of frames within 1 ms either way.
- **Native resolution:** the frame time is the game thread's here and doesn't change, 6.79 to 6.97 ms in every segment, on and off, both APIs; the resolve time saved (timestamps above) is what a GPU-bound system gains.

Checks:

- **Bit-exact comparison** (`launcher/dev/direct-resolve-validate.sdk.patch`, with `rexglue-resolve-4xmsaa-depth-samples.patch` in): one direct resolve at a time was also done through the EDRAM buffer into the same destination (the scaled resolve buffer, or shared memory at native) and both results compared, in real time through the attract movie, the map zoom-out, a minute standing in the city by day, a drive, and the race in the rain: 162,550 resolves at native, 2x and 3x on both APIs, 9,942 of them 4x depth and 6,584 full 16bpp, with no differing byte in any. Without the other patch, every kind matched except the 4x MSAA depth resolves at 3x (255 of 263 in the city on D3D12 differed), the EDRAM path's misreads.
- **Screenshots** with the setting switched on and off within one run (the dev pad's `set`): at 3x on both APIs, the attract movie, the map zoom-out, standing in the city, a drive and the race (rain, opponents, HUD) look the same either way. In the pause menu standing in the city, eight shots alternating, on and off are identical at every pixel that stays the same from shot to shot, at native and at 3x on both APIs.
- **D3D12 debug layer** through the city and the race, at native and 3x: no errors; the only warnings are unrelated to resolves (initial states ignored for buffers, blend factors with MIN/MAX, live objects at exit).
- **Vulkan validation** with synchronization validation through the city and the race, at native and 3x: `ShaderOutputNotConsumed`, `vkCmdEndQuery` 07007 and 01923, and write after write hazards only between compute dispatches into the texture loads' scratch buffer or into the EDRAM buffer (dumps and other stores to it), the same kinds with the setting off: none on a render target, a resolve destination or a direct resolve pipeline. The layer reports a message only 10 times by default, which would hide later hazards of the same kind, so the city and the race were run again with `VK_LAYER_ENABLE_MESSAGE_LIMIT=0` (thousands of messages, the same kinds). The race now gets there under synchronization validation: in the first round's run the menus, at 10 to 20 FPS, ran out of step and it never started (`boot-to-race.pad` now also waits in game updates).
- **The attract movie** plays through its resolves on both APIs (the bit-exact comparison above covered its 40 s).

Built and run on Windows only, with an NVIDIA RTX 5080.

| Change | Files | Affects |
|---|---|---|
| Choosing the render target and the offset | `include/rex/graphics/pipeline/render_target/cache.h`, `src/graphics/pipeline/render_target/cache.cpp` | D3D12, Vulkan |
| The root signature, null views, pipelines, the resolve | `include/rex/graphics/d3d12/render_target_cache.h`, `src/graphics/d3d12/render_target_cache.cpp` | D3D12 |
| The pipeline layout, dummy images, descriptor sets, pipelines, the resolve | `include/rex/graphics/vulkan/render_target_cache.h`, `src/graphics/vulkan/render_target_cache.cpp` | Vulkan |
| The shaders (XeSL sources, DXBC and SPIR-V) | `src/graphics/shaders/resolve_direct*`, `src/graphics/shaders/bytecode/d3d12_5_1/resolve_direct_*_cs.h`, `src/graphics/shaders/vulkan_spirv/resolve_direct_*_cs.h` | D3D12, Vulkan |

## rexglue-cvar-toml-strings.patch

`SaveConfig` (the SDK's F4 settings overlay saves with it) wrote a string option between double quotes as it was. A Windows path came out as `"C:\Users\..."`, where `\U` starts a TOML escape, so the whole file failed to parse at the next start and every setting in it was lost for that run (the file itself stayed). Any path option given on the command line, such as `--log_file`, ended up in the file this way at the next save. And it rewrote the whole file from every option's current value: command-line options and an app's own defaults went into the file, and comments, unknown keys and tables were lost.

Now:

- **Strings** are written as TOML basic strings with `\`, `"` and control characters escaped.
- **Saving merges** into the file (`MergeIntoToml`): each key's `key = value` statement is replaced where it is, keeping a comment after it and dropping the rest of a value that spanned several lines, or added before the first `[table]`. Every other line is kept: comments, unknown keys, tables, multi-line strings and arrays (a `key = ` line inside one is not taken for a key), quoted and dotted keys, a BOM, CRLF. A one-line value of the old broken form is rewritten as the same text in a literal string, so the file parses again. A file that exists but can't be read is not written.
- **Writing is durable** (`WriteFileDurably`): the text goes to `<file>.<pid>.tmp` beside it, is flushed to the disk (`_commit` / `fsync`), and renamed over the file; any failure leaves the old file and reports it.
- **`SaveConfig`** (F4's *Save to config*) merges only the options changed while running (`Source::kRuntime`): not what came from the command line, the file or an app's defaults.
- **`bool SaveConfigValues(path, names)`**, new: merges just these options, whatever their source, and says whether it worked. The game's pause menu saves with it.
- **`bool SetDefaultValue(name, value)`**, new: an app's own default for an option, applied unless the command line or the file sets it, and never saved by `SaveConfig`. The game's PC defaults use it.
- **A file that still fails to parse** (one saved before this, or edited by hand) is read again statement by statement instead of not at all: each statement that parses on its own applies, with its `[table]` prefix (a BOM before the first one is skipped), and nothing is read from inside a multi-line string or array. A double-quoted value that fails only for its backslashes is taken as written, as a literal string, unless its text holds a quote (then it is not guessed at). Escapes are checked as TOML defines them: `\u` and `\U` must name a Unicode scalar value. Statements that still fail are skipped with a warning.

Checked with a small program against the built `rexruntime.dll` (21 checks): a path, a string with quotes and a tab, and a number saved and loaded back unchanged; F4's save leaves out a command-line value and an app default, and keeps a comment after a replaced value, an unknown key and a table; a quoted key replaced in a file with a BOM and a multi-line string holding a `key = value` line; a file with an old-style path, a quoted value with a quote in it, a line that is not TOML, keys inside a multi-line string and an array, and tables, with and without a BOM. In the game, the pause menu saved into such a file and the next start read it. Built and run on Windows only; the PS5 host's `LoadConfig` of `/data/mcla/mcla.toml` goes through the same code.

| Change | Files | Affects |
|---|---|---|
| Strings saved as escaped TOML; saves merged into the file and written durably; F4 saves only what changed while running; `SaveConfigValues` and `SetDefaultValue`; a file toml++ rejects read statement by statement | `src/core/cvar.cpp`, `include/rex/cvar.h` | all platforms |

## rexglue-edram-size.patch

**Why.** The Xbox 360's EDRAM is 2,048 tiles (10 MiB), and the SDK emulates exactly that, with addresses wrapping around at its end. A title drawing 720p with 2x MSAA has to split the scene into predicated tiles and submit it once per tile; this game draws two (1280x512 and 1280x208), each replaying about 1,370 draws in the city. Patched to draw one tile, it needs 1,472 tiles for the colour and as many for the depth, more than the console had (`launcher/dev/single-tile-2026-10-16.md`).

**What it does.** A setting, `edram_tiles` (GPU options, 2048 or 4096, read at start-up), sets the size of the EDRAM emulated. 2048, the console's, is the default. With 4096:
- EDRAM addresses wrap at 4,096 tiles, and render target bases are 12 bits (`RB_COLOR_INFO` and `RB_DEPTH_INFO` through `GetBaseTiles(edram_tile_count)`, `RenderTargetKey`, Vulkan's framebuffer keys). The ownership map, its wrap-around, the transfers and the resolve areas use the size in effect (`RenderTargetCache::edram_tile_count`).
- The EDRAM buffer is twice the size: 20 MiB at 1x, 80 MiB at 2x, 180 MiB at 3x. A backend that can't bind that much whole (D3D12: 2^27 32-bit elements, 512 MiB; Vulkan: `maxStorageBufferRange`) gets the console's 2,048 tiles, with a warning in the log.
- Host render targets stay as tall as the console's EDRAM from their base, as before, so they take no more memory.
- The transfer shaders' address constant packs the two pitches in 9 bits each instead of 10 (a render target's pitch is at most 255 tiles at 32bpp, 510 at 64bpp), so that the difference of two 12-bit bases fits beside them.
- **Resolve shaders.** The 22 EDRAM buffer resolve shaders per API (fast, full and clear, unscaled and scaled) wrap at 10 MiB and read an 11-bit base. Their copies for 4,096 tiles are built from the same xenia-project/xenia 0b2ffa3 sources with two functions redirected (`src/graphics/shaders/resolve_edram4096.xesli`, included first by 22 `resolve_*_edram4096.cs.xesl` entry files): `XeEdramOffsetInts` wraps at 4,096 tiles, and `XeResolveGetInfo` also takes base bit 11, which the host passes in bit 30 of `xe_resolve_edram_info` (`ResolveEdramInfo::base_tiles_bit_11`; bits 0 to 29 are as before). Built with fxc and glslangValidator with spirv-opt as the fork's other shaders; the original 22, built the same way, came out identical to the SDK's. With the larger EDRAM their pipelines are created instead of the console ones. The direct resolve shaders read render targets, not the EDRAM buffer, and are unchanged.
- **ROV and FSI.** The shader translators get the size from the pipeline caches and wrap the pixel shaders' EDRAM addresses at it; the command processors pass 12-bit bases.
- **For the title:** `IGraphicsSystem::edram_tile_count()`, the size in effect (0 until the GPU has started), which the command processors set once their render target cache has chosen. The interface changed, so `kGpuPluginAbiVersion` is 2.

`xenos::kEdramTileCount` and `kEdramSizeBytes` are gone, so that every use had to be looked at: `kEdramTileCountConsole` (2,048), `kEdramTileCountMax` (4,096) and `GetEdramSizeBytes(count)` replace them.

**At 2,048 tiles too,** two changes apply, though this game reaches neither (with 2,048 tiles nothing in it crosses tile 2,048, `launcher/dev/edram-wrap-log.sdk.patch`):
- a resolve clear whose area wraps round the end of the EDRAM was dropped: `PrepareHostRenderTargetsResolveClear` subtracted the render target's base from the resolve's without the wrap-around, so the offset underflowed. It is now taken modulo the EDRAM size. The same bug is in xenia;
- the transfer shaders made at run time pack their pitches in 9 bits, at either size.

A title that relied on the console's wrap-around at 2,048 tiles would draw differently with 4,096, which is why it is a setting, off by default. This game sets it on PC (`MclaApp::ApplyPcDefaults`) and on the PS5 (`ps5/game/main_ps5.cpp`, untested there) for its single-tile hook (`src/render_perf.cpp`).

Checked on Windows, RTX 5080, with the game's single tile (`launcher/dev/single-tile-edram-2026-10-16.md`):
- **Resolves:** `launcher/dev/direct-resolve-validate.sdk.patch` compared direct resolves with the same resolves through the EDRAM buffer and the 4,096-tile shaders (the one-tile scene's colour crosses tile 2,048): 33,700 at native and 3x, city and race, both APIs, none different.
- **Wrap-around:** `launcher/dev/edram-wrap-log.sdk.patch` logged every EDRAM range crossing tile 2,048. With 4,096 tiles, only the one-tile scene's colour and its resolve; with 2,048 tiles, none in this game.
- **The picture,** one tile against two within one run, at 1x, 2x and 3x on both APIs, and on the EDRAM buffer, ROV and FSI paths: the same within frame-to-frame noise.
- **D3D12 debug layer:** no errors, the usual warnings. **Vulkan synchronization validation:** the same write-after-write hazards between compute dispatches as with 2,048 tiles.
- **With 2,048 tiles** (`--edram_tiles=2048`): the game keeps two tiles whatever its option says, and a switching run (50 switches each way, menus, the GPS map) behaved as before; with this size the SDK uses the console's resolve shaders and limits.

Built and run on Windows only, with an NVIDIA RTX 5080. The PS5 build applies the patch and asks for 4,096 tiles, untested on the console (both changes above that apply at 2,048 tiles run there too).

| Change | Files | Affects |
|---|---|---|
| The setting, the size chosen, ownership and resolve areas, 12-bit keys, 9-bit transfer pitches | `src/graphics/pipeline/render_target/cache.cpp`, `include/rex/graphics/pipeline/render_target/cache.h`, `include/rex/graphics/xenos.h`, `include/rex/graphics/registers.h`, `include/rex/graphics/util/draw.h`, `src/graphics/util/draw.cpp` | D3D12, Vulkan |
| The EDRAM buffer, resolve pipelines, transfer and dump shaders, framebuffer keys | `src/graphics/d3d12/render_target_cache.cpp`, `include/rex/graphics/d3d12/render_target_cache.h`, `src/graphics/vulkan/render_target_cache.cpp`, `include/rex/graphics/vulkan/render_target_cache.h` | D3D12, Vulkan |
| ROV and FSI addressing | `src/graphics/pipeline/shader/dxbc_translator*.cpp`, `src/graphics/pipeline/shader/spirv_translator_rb.cpp`, their headers, `src/graphics/{d3d12,vulkan}/pipeline_cache.cpp`, `src/graphics/{d3d12,vulkan}/command_processor.cpp` | D3D12, Vulkan |
| The 4,096-tile resolve shaders (XeSL sources, DXBC and SPIR-V) | `src/graphics/shaders/resolve_edram4096.xesli`, `src/graphics/shaders/resolve_*_edram4096.cs.xesl`, `src/graphics/shaders/bytecode/d3d12_5_1/resolve_*_edram4096_cs.h`, `src/graphics/shaders/vulkan_spirv/resolve_*_edram4096_cs.h` | D3D12, Vulkan |
| `IGraphicsSystem::edram_tile_count`, the plugin ABI version | `include/rex/system/interfaces/graphics.h`, `include/rex/system/gpu_plugin.h`, `include/rex/graphics/graphics_system.h` | all platforms |

## rexglue-guest-markers.patch

**Why.** HDR output has to know where the game's frame reaches the end of its 3D scene, its tone map and its UI, and nothing in the GPU command stream says so. The game writes markers there (`src/render_markers.cpp`): PM4 type-3 NOP packets of three words, `'REXM'`, a kind and a value.

**What it does.** `CommandProcessor::ExecutePacketType3_NOP` passes a 3-word NOP with that magic to a new virtual, `OnGuestMarker(kind, value)`; every other NOP is skipped as before. The kinds are `GuestMarkerKind` (frame start and end, scene end, tone map, UI, render loop end, and a phase bit for diagnostics). Markers in a predicated-tiled pass are replayed once per tile, so what they start must be idempotent. A title that writes no markers sees no change. The HDR patches after it use them; on its own it does nothing else.

Checked: screenshots with the game's markers on and off (`mcla_render_markers`) identical at every still pixel at 3x on both APIs.

| Change | Files | Affects |
|---|---|---|
| The `'REXM'` NOP and `OnGuestMarker` | `include/rex/graphics/command_processor.h`, `src/graphics/command_processor.cpp` | all platforms |

## rexglue-hdr-output.patch

**Why.** The presenter only had SDR swap chains. HDR displays show SDR at the desktop's SDR level, so nothing the game draws above its white can be shown.

**What it does.** A setting, `hdr` (`off`, the SDK's default; `auto`, when the display is in HDR mode, which this game makes its default on Windows; `on`, also tries without), makes the swap chain HDR10: D3D12 `R10G10B10A2_UNORM` with `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` (`CheckColorSpaceSupport`, `SetColorSpace1`; a switch is `ResizeBuffers`), Vulkan `A2B10G10R10_UNORM_PACK32` with `VK_COLOR_SPACE_HDR10_ST2084_EXT` (the instance extension `VK_EXT_swapchain_colorspace`, optional). While it is, the swap's refresher is `hdr_refresh.cs` instead of the gamma pass: SDR values go through the guest's own 256-entry display table as before, so a game's gamma setting means the same in HDR (MCLA's DISPLAY GAMMA switches during play), are decoded as SDR (`hdr_sdr_eotf`: the piecewise sRGB curve, as Windows shows SDR on an HDR desktop, or a 2.2 power), placed in nits, rolled off above 0.75 of the peak towards it on the largest component, and written as PQ BT.2020 code values into the guest output. The presenter only scales that (bilinear: FSR, CAS and dithering assume SDR and are skipped), draws its own UI (ImGui, toasts) into an 8-bit layer cleared to 0 and composites it over the PQ image at the SDR level (`hdr_ui_composite.ps`), and captures turn HDR10 back into SDR relative to the SDR white level (SDR content within one level; light above it clipped keeping its hue, the roll-off not undone).

Levels: `hdr_peak_nits` and `hdr_paper_white_nits` (0 = what the display reports: DXGI `IDXGIOutput6::GetDesc1`, which includes the Windows HDR Calibration profile, and DisplayConfig's SDR white level), `hdr_ui_nits` (SDR content; 0 = paper white). `hdr_test_pattern` shows bars from 100 nits doubling every 4 bars, not rolled off (where the display stops showing steps), and paper white with the rolled-off bars below. `hdr_debug_ui` has debug views (the later patches add to them). The refresher learns the state through `Presenter::GetHdrOutputState`, and tells the presenter what it wrote through `GuestOutputRefreshContext::SetHdr`. The GPU plugin ABI is 3 (4 after `rexglue-hdr-ui-layer.patch`).

Checked on Windows, RTX 5080, both APIs: switching SDR, HDR, SDR with still pictures identical before and after; the capture round trip within one level on 99.92% of pixels; the test pattern the same on both APIs; the refresher 60 us at 3x against 35 us for the SDR pass; the D3D12 debug layer and Vulkan validation with nothing new.

| Change | Files | Affects |
|---|---|---|
| Settings, the display's levels, the HDR output state, the capture conversion | `include/rex/ui/presenter.h`, `src/ui/presenter.cpp`, `src/ui/window_sdl.cpp` | all platforms (the levels are read on Windows only) |
| HDR10 swap chains, the final pass, the presenter's UI layer | `src/ui/d3d12/d3d12_presenter.cpp`, `include/rex/ui/d3d12/d3d12_presenter.h`, `src/ui/vulkan/vulkan_presenter.cpp`, `include/rex/ui/vulkan/presenter.h`, `include/rex/ui/vulkan/vulkan_instance.h`, `src/ui/vulkan/vulkan_instance.cpp` | D3D12; Vulkan |
| The refresher and its pipeline | `src/graphics/{d3d12,vulkan}/command_processor.cpp` and headers | D3D12; Vulkan |
| Shaders (XeSL sources, DXBC and SPIR-V) | `src/ui/shaders/hdr_output.xesli`, `src/ui/shaders/hdr_ui_composite.ps.xesl`, `src/graphics/shaders/hdr_refresh.cs.xesl`, their headers under `bytecode/d3d12_5_1` and `vulkan_spirv` | D3D12; Vulkan |
| ABI | `include/rex/system/gpu_plugin.h` | all platforms |

## rexglue-hdr-ui-layer.patch

**Why.** In HDR the scene and the game's UI need different levels (the UI at the SDR level, the scene free to go above it), but the game draws its HUD and menus into the same 8-bit target as its tone-mapped scene.

**What it does.** From the UI marker, the render target the UI phase draws to (`8_8_8_8`, 1x or 4x MSAA) is copied (the underlay) and cleared to 0 before its first blended draw, once per tile of a tiled pass; draws blended `SrcAlpha` or `One` / `InvSrcAlpha` get the alpha blend `One` / `InvSrcAlpha` (in the pipeline key on both APIs), so the target collects the UI premultiplied with its coverage in alpha. When the game resolves that target to the front buffer, the UI and the underlay are copied to screen-space textures at the resolve's place in the front buffer (its destination base's tiled position plus its offset; separate x and y scales) and the target is put back for the game's resolve with one blended draw (`hdr_underlay_restore.ps`, `InvDestAlpha` / `One` for colour and alpha: UI + underlay x (1 - alpha)), so the game's own 8-bit frame, which it reads back in its menus, is what it was (its colour; the alpha put back, A + U.a x (1 - A), equals the game's own only where the game blends alpha `One` / `InvSrcAlpha`, and nothing in MCLA reads it). The refresher then blends the UI over the scene in the game's code values, as the game blends it, before the display table. Another blend, another format, a failed copy (no host render targets: ROV, FSI) or the target resolved somewhere else mid-UI end the separation for the frame (the target is put back at once, and the draw keeps its own blend); the frame then falls back to the difference between the front buffer and the underlay, or, if the target never reaches the front buffer, to the whole frame as SDR, each cause logged once. A frame with the tone map but no UI pass is all scene; one without the tone map stays all SDR. On Vulkan, render targets are copied by a compute shader through their sampled view (4x averaged in the shader, `hdr_rt_copy*.cs`), so they get no extra usage. `hdr_ui_layer = false` turns it off (the whole frame as SDR).

It also carries the fixes from the review of the three HDR patches: the display's levels read on a thread of their own (the paint and GPU threads only copy them) and once before the first connection, no HDR10 swap chain when the guest output can't be PQ, a reconnection when the display's HDR state changes (also with `hdr = on`), HDR10 metadata (`SetHDRMetaData`, `VK_EXT_hdr_metadata` where present: BT.2020, D65, the peak as MaxCLL and mastering maximum), the immediate drawers' alpha `One` / `InvSrcAlpha` (so the presenter's UI layer is premultiplied; the swap chain's alpha is ignored in SDR), one descriptor request in the D3D12 refresher, and the Vulkan presenter waiting for the old swapchain's last present before destroying it on a recreation (an upstream bug, `VUID-vkDestroySwapchainKHR-swapchain-01282`, which HDR switches made common). The GPU plugin ABI is 4.

Checked on Windows, RTX 5080, both APIs, at 3x: every scene reached took the layer with no fallback (`launcher/dev/hdr-2026-10-16.md` has the list); the target put back against the game's frame without the layer: pause 94.5% of still pixels exact and 99.99% within 2 levels, GPS map 85.1% and 99.94%, free roam 97.2% exact, the same on both APIs; no draw left out while the new pipelines compiled; the D3D12 debug layer and Vulkan synchronization validation with nothing new; with `hdr` off, boot frames byte-identical to the build without it.

| Change | Files | Affects |
|---|---|---|
| The UI layer's state, the draw and resolve hooks, the composite choice | `include/rex/graphics/command_processor.h`, `src/graphics/command_processor.cpp`, `include/rex/graphics/pipeline/render_target/cache.h` | D3D12, Vulkan |
| Copies, clears and the restore on host render targets; the textures; the refresher's inputs | `src/graphics/d3d12/{command_processor,render_target_cache,deferred_command_list}.cpp` and headers, `src/graphics/vulkan/{command_processor,render_target_cache,deferred_command_buffer}.cpp` and headers, `include/rex/ui/vulkan/functions/device_1_0.inc` | D3D12; Vulkan |
| Shaders | `src/graphics/shaders/hdr_fullscreen.vs.xesl`, `hdr_underlay_restore.ps.xesl`, `hdr_rt_copy.cs.xesl` and `hdr_rt_copy_4xmsaa.cs.xesl` (Vulkan), `hdr_refresh.cs.xesl`, their headers | D3D12; Vulkan |
| Review fixes in the presenters and devices | `src/ui/presenter.cpp`, `include/rex/ui/presenter.h`, `src/ui/{d3d12,vulkan}/*presenter.cpp`, `src/ui/{d3d12,vulkan}/*immediate_drawer.cpp`, `src/ui/vulkan/vulkan_device.cpp`, `include/rex/ui/vulkan/device.h`, `include/rex/ui/vulkan/functions/device_ext_hdr_metadata.inc` | D3D12; Vulkan |
| ABI | `include/rex/system/gpu_plugin.h` | all platforms |

## rexglue-hdr-scene-mcla.patch

**Why.** With the UI apart, the scene under it is still the game's 8-bit output: its tone map clips at its white. The inputs of that tone map (the HDR scene, bloom, blur, the adapted luminance and its constants) are all there at the tone map draw. This patch is for Midnight Club: Los Angeles only.

**What it does.** The colour draws between the tone map and UI markers are looked through for the tone map's shaders (VS `1FAB9787BCA32D7C`, PS `E5EBEFAE4D165CBA`); that draw must also go to a 1x `8_8_8_8` target, cover the whole output (viewport, scissor, no window offset) and have the texture fetch constants the pass was written against (2D, the game's formats, signs, swizzles, edge modes and filters, no exponent bias, the scene at the output's size). Otherwise, or with no such draw, the frame keeps the 8-bit scene; each change of the outcome is logged with the shaders' hashes and the reason. The pass doesn't run where its copies can't (no host render targets: ROV, FSI) or in the debug views that don't show the scene. After that draw, `hdr_scene_mcla.cs` runs the game's operator again from its textures (pixel shader fetches 0 to 2, the vertex shader's fetch 16; 2D arrays, as the texture caches bind them) and constants, sampling where the draw samples (its half-pixel offset at the resolution scale), and writes the scene in the game's linear units to an FP16 texture: the game's Reinhard curve up to a knee (`mcla_hdr_knee`, 0.6 of its white), then an exponential shoulder with the same value there towards the peak in the game's units, its slope the game curve's times `mcla_hdr_highlights` (1, 1.5, 2 or 3; 2, Brighter, by default) but at least what reaches the game's white at the game's white, and never under the game's curve below its white (by day the game's curve bends up towards its white, where a shoulder from the same slope would stay under it); with a peak at or below the game's white, the game's curve clipped at its white. Then bloom (`mcla_hdr_bloom_gain`), saturation, colour balance and contrast as the game does. The draw's output is copied too: where the 8-bit picture under the UI is that output, or brighter by light added after it (the night glows), the refresher takes the scene from the FP16 texture plus that light (its 8-bit difference, times the highlights setting), through the game's encode, the display table and the SDR decode, all extended above 1; anything darker drawn over it keeps the 8-bit picture. The added light takes the highlights setting only when every draw on the tone map's target between it and the UI added light (dest factor `One`, `Add`: the glows, `One` / `One`); a frame with any other draw there (a flash, a fade, a panel) keeps that difference at its 8-bit level, logged when it changes. NaN from a guest texture or constant doesn't reach the output (the scene's luminance is clamped, the pass's output and the refresher's table index are checked by their bits); the adapted luminance is read at its texture's middle texel (3x3 at 3x); a frame's HDR scene is only used at the output size it was made at. The shoulder's top, mapped to the peak, and the presenter's roll-off from 0.75 of the peak together keep the brightest highlights of the scene at about 90% of the peak setting; bloom and contrast take some past that, towards the peak. The HDR textures (about 200 MB at 3x) are released two seconds or so after HDR output goes off, once the GPU is done with them, and made again when it comes back. The presenter gains `RefreshHdrDisplayInfo` (reads the display now, also with `hdr` off, for a settings screen being opened) and `GetHdrAutoLevels` (what AUTO stands for: the report, or 1000 and 200 nits), and its display thread no longer waits on its lock when rexruntime is unloaded at exit (the thread may have been ended holding it). Debug views: `hdr_debug_ui 7`, the pass with the game's own curve shown as the game shows it; 8, the output's HDR10 code values as grey levels; 1 shows the scene where it isn't HDR in cyan. `mcla_hdr_scene = false` turns it off.

Checked on Windows, RTX 5080, both APIs, at 3x: the game's curve through the pass against the game's frame, 86.5% of pixels identical, 95.5% within 1 level, 97.3% within 2 (the rest: columns where the GPU's sample lands on the neighbouring texel, a third of a game pixel, and clipping in the brightest areas); the 8-bit fallback under 0.1% of pixels in every scene; levels with paper white 240 and peak 1317: day 424 nits at most, dusk 814, night city 456, night race 1167, the highlights steps raising a dusk scene's maximum from 785 to 1209 nits with the median unchanged; all HDR work per frame 0.48 ms on D3D12 and 0.56 ms on Vulkan (the pass 0.18 ms); the D3D12 debug layer (bindless and bindful) and Vulkan synchronization validation with nothing new; with `hdr` off nothing of it runs. After the second review's fixes: the shoulder at Normal no longer under the game's curve near its white (the brightest pixel of a paused dusk frame 1086 to 1125 nits at peak 1317 and paper white 280, a day frame 875 to 975; at peak 600 and paper white 240, 547 to 567 and 509 to 527), Brighter unchanged in the same frames; every scene of the list on the UI layer and the tone map found, on both APIs; all HDR work per frame at 3x 0.51 ms on both APIs (the pass 0.20 ms); the D3D12 debug layer and Vulkan synchronization validation, also through the textures' release and HDR back on, with nothing new.

| Change | Files | Affects |
|---|---|---|
| The tone map check, the constants, the shoulder, the copy of the draw's output, the glows' gain, releasing the textures | `include/rex/graphics/command_processor.h`, `src/graphics/command_processor.cpp` | D3D12, Vulkan |
| The pass and its inputs; the refresher's scene inputs; releasing the textures | `src/graphics/{d3d12,vulkan}/command_processor.cpp` and headers | D3D12; Vulkan |
| Shaders | `src/graphics/shaders/hdr_scene_mcla.cs.xesl`, `hdr_refresh.cs.xesl`, their headers | D3D12; Vulkan |
| `hdr_debug_ui` views 7 and 8; reading the display on request, AUTO's levels, the display thread's shutdown | `include/rex/ui/presenter.h`, `src/ui/presenter.cpp` | all platforms (the display is read on Windows only) |

The HDR shaders are built from their XeSL sources with xenia-project/xenia's 0b2ffa3 shader tree, fxc (`/O3`) and glslangValidator with spirv-opt, into headers of 32-bit words without the disassembly (`launcher/dev/hdr/build_shaders.py`). All four HDR patches are built and run on Windows only; the PS5 build applies them but has not been compiled with them, and HDR stays off there.

## rexglue-temporal-aa.patch

**Why.** The game's own anti-aliasing is 2x MSAA, which smooths the outlines of geometry but not what is inside it: the shimmer of fences, power lines, foliage, distant detail and specular highlights, which moves with every step of the camera. Temporal anti-aliasing accumulates the scene over frames, each offset by a fraction of a pixel, and calms all of it. This patch adds AMD FSR 3.1 at native resolution (FidelityFX SDK 1.1.4, MIT) as an option; with it off, nothing changes. The generic parts (markers, jitter, callbacks, FSR's integration, the Vulkan features) are for any title; what finds the anti-aliasing point and reads its constants is Midnight Club: Los Angeles's.

**What it does.**

- **Markers.** `GuestMarkerKind` gains `kSceneBegin` (7), `kSceneTranslucent` (8) and `kSceneComposite` (9); the game writes them at its render phases 0x800 (the main view's scene), 0x100000 and 0x04000000 (`src/render_markers.cpp`).
- **Jitter.** With `temporal_aa = fsr` and FSR ready, the draws between `kSceneBegin` and `kSceneEnd` that are drawn through the viewport into the main view (not rectangle lists, clipping on, viewport scale and offset on, the viewport as wide as the target) are moved by FSR's sub-pixel jitter (Halton 2, 3, 8 phases) through the host viewport's NDC offset (`CommandProcessor::TaaJitterNdcOffset`, called in both backends' `IssueDraw` after the viewport is known; the viewport cache keeps it without). Everything before the scene (reflections, shadows, impostors), the fog quad, post-processing and the HUD is not moved. A frame is jittered only after one in which the pass ran, so a frame without it (a menu, a loading screen, a frame without the motion blur draw) never shows the shake; and FSR is told a jitter of zero for a frame in which no draw was moved.
- **The anti-aliasing point** (`TaaCheckDraw`, MCLA): the game's motion blur draw (pixel shader `A6B9B6B0FA692C54`, between `kSceneEnd` and `kTonemapBegin`, once in every 3D frame, also with MOTION BLUR and FULLSCREEN BLUR off), whose fetch constants are the scene (0, `16_16_16_16_EXPAND`), the stencil view of the depth resolve (1, `8_8_8_8`, the vehicle ID in `x >> 2`) and the depth (2, `24_8_FLOAT`, 1 near, 0 far), all at the frame's size (its `c62`; a stencil or depth of another size, and the pass doesn't run). From its pixel shader constants: the view-projection (`c8-c11`, inverted in double precision) times the previous frame's view-projection to pixels for each vehicle ID (`c69 + 4k`, k < 24), a matrix per ID that takes a pixel's clip position to where it was a frame before, with the camera's and each car's motion; near, far and the field of view from the projection (`c4-c7` the view); a cut (the camera moved more than `temporal_aa_cut_distance`, 25 units, turned more than 30 degrees, or the field of view changed by more than 5%) drops FSR's history.
- **The pass**, before that draw is recorded, after its textures are requested (`TaaRun` in `d3d12/temporal_aa.cpp` and `vulkan/temporal_aa.cpp`): `taa_motion_vectors_mcla.cs` makes the motion vectors (R16G16 float, host pixels, the jitter taken out), FSR runs over the scene texture with them and the depth (HDR input, inverted depth, its own exposure), and its output is copied into the texture cache's texture of the scene, so the motion blur, the tone map and HDR output all read the anti-aliased scene and are unchanged. The copy takes the scene's alpha too (FSR writes 1): nothing reads it after this point (the motion blur, the tone map and the HDR pass fetch only its colour, and the downsamples for the exposure and the bloom run before it).
- **Reactive mask (experimental, off by default).** With `temporal_aa_reactive` above 0, the scene's colour target is read at `kSceneTranslucent` and `kSceneComposite` (`taa_luma_snapshot.cs`, luminance to R16 float) and `taa_reactive_mcla.cs` makes FSR's reactive mask from how much the translucent effects drawn between them (rain, smoke, particles, glows) changed it: the relative change of luminance times the setting, from 0.03 up, at most 0.9, and 0 on the vehicles (stencil ID above 0). It is off by default: at 3x on D3D12, FSR without it showed no trails on the smoke plumes in the night race or on tire smoke in a burnout and a drift in free roam (alike to MSAA's), while with 4 it marked stable things whose history FSR should keep, the headlights' pools of light under a standing car and a sunlit wall at dusk (`temporal_aa_debug 5` shows it). It marks what the translucent phase changed in a frame, not what changed between frames.
- **FSR in the command stream.** The deferred command list (D3D12) and buffer (Vulkan) gain an external callback command: a function and a copy of its data, called with the real command list where it falls, on the thread that executes them (threaded submission included). FSR is dispatched from there; afterwards the command processor sets its descriptor heaps again and binds its pipelines, root signatures and descriptor sets afresh. FSR's context is made on a thread of its own the first time (half a second or so), and the frames until it is ready are left as they are.
- **Failures and release.** If FSR's dispatch fails, the callback copies the scene into the output, so that frame is left as it was, and FSR is marked unavailable; a context that can't be made marks it unavailable too. FSR's context and resources are released when `temporal_aa` has been off for 10 seconds, when the frame's size changes (recreated at the new size) and after a failure, never in the middle of a draw: at the end of the frame, with the context and the images destroyed once the submissions that may use them are done.
- **Availability.** `temporal_aa_available` (`unknown`, `yes`, `next_start`, `no`, set by the plugin at the first frame): `yes` where the runtime for the graphics API in use is next to the executable (`amd_fidelityfx_dx12.dll`, `amd_fidelityfx_vk.dll`, on Linux `libamd_fidelityfx_vk.so`; on the PS5 linked in) and, on Vulkan, the device is 1.3 with the storage formats FSR writes and was made with FSR's features (`next_start` when only that is missing: `temporal_aa_device_features` was off); `no` otherwise and after a failure, logged once. The game shows FSR only while it isn't `no` and uses MSAA in its place; the launcher shows it only where the DLL for the chosen API was built.
- **The runtime.** `cmake/rexglue_fsr.cmake` (`REXGLUE_ENABLE_FSR`, which `scripts/make_pc.ps1` turns on) fetches the FidelityFX SDK at a pinned commit of the ReXGlue fork (`eee08db`, AMD's v1.1.4 with portability fixes) into a temporary folder in the build tree, checks the tree hashes of its `ffx-api` and `sdk` folders, and only then moves it into place (a checkout that fails the check is fetched again once); it builds `amd_fidelityfx_dx12.dll` and `amd_fidelityfx_vk.dll` (the latter with the Vulkan SDK) as external projects whose wrapper and outputs are all in the build tree. FidelityFX compiles its shaders with its own FidelityFX_SC.exe, a Windows program: elsewhere (`REXGLUE_FSR_PREBUILT_SHADERS`, on by default off Windows) the Vulkan runtime is built from the prebuilt headers of `rexglue-temporal-aa-shaders-vk.patch`: right after FidelityFX's `project()` for the Vulkan backend, when it has picked its compiler, a hook (`cmake/fidelityfx/prebuilt_shaders_hook.cmake`, through `CMAKE_PROJECT_FidelityFX-SDK_VK_x64_INCLUDE`) puts `cmake/fidelityfx/prebuilt_shaders.cmake` in its place, which copies each pass's prebuilt table and blobs to where the compiler's output would go and stops the runtime's build on a set that isn't there (FidelityFX's `FFX_API_AUTO_COMPILE_SHADERS=OFF` alone doesn't keep its backend from running the compiler), and the GPU plugin refuses to create FSR from them with create flags other than those they were made for (logged; MSAA instead). On Linux the runtime is `libamd_fidelityfx_vk.so`, built position-independent against the SDK's own Vulkan headers (no development package or minimum version needed) and linked to the system's Vulkan loader; `scripts/play_linux.sh` links it next to the game. On the PS5, which can't load modules, ffx-api's sources are built as a static library beside FidelityFX's own (`cmake/fidelityfx/CMakeLists.txt`, `REXGLUE_FSR_STATIC`): the shared target's sources and properties, its folder's compile definitions (where ffx-api selects its backend, `FFX_BACKEND_VK`) and its link libraries' usage requirements (the SDK's Vulkan headers, through `cmake/fidelityfx/vulkan-headers-only/FindVulkan.cmake`), with the SDK's code model (`-mcmodel=large`: an LTO link refuses mixed ones). `cmake/fidelityfx/check_static.cmake` fails that build when the archive doesn't refer to `ffxGetInterfaceVK` (built without its backend, ffx-api still links but creates nothing); the archives go to the SDK's archive folder, which the game links. The plugin refers to FidelityFX weakly (`REX_FSR_STATIC`), and `src/graphics/vulkan/temporal_aa_static.cpp` defines the eight Vulkan functions FidelityFX refers to directly (seven it calls and `vkGetDeviceProcAddr`, whose address it takes), which a loader would export, forwarding them to the plugin's instance and device. `ps5/make_ps5.sh` (`MCLA_PS5_FSR=0` leaves FSR out) checks with `llvm-nm` that the archives refer to no Vulkan function that neither the plugin nor the driver defines, and drops them if they do; `ps5/game/build.sh` links the title with `--undefined=ffxCreateContext` to take them and, if that link fails, links it again without them (with `-z nodynamic-undefined-weak`, so the weak references never become dynamic imports), warning that the game won't offer FSR. A new prebuilt set reaches existing build trees: its hash is in the runtime's configure command, the stand-in's dependency files name the prebuilt files, and every switch of the wrapper is passed, empty or off when not used, so that its cache keeps none. A `static_assert` checks the plugin's create flags against the set's, and the wrapper stops when the hook didn't run (a renamed FidelityFX project). A missing blob would still reach `vkCreateShaderModule` with no code (FidelityFX only asserts, compiled out): unreachable with these flags; making it a hard failure needs a change to FidelityFX, for when the pin next moves. Every step is soft: without Git or the network, with a failed check, or if FidelityFX's configure or build fails, CMake warns and the SDK builds without FSR (or without it on Vulkan), and the game offers MSAA and off only. `make_pc.ps1` rebuilds the SDK when the DLLs are missing, copies them next to `mcla.exe`, and lists the Vulkan SDK as an optional tool (installed with winget under `-InstallTools`). The ffx-api headers are in the patch, unmodified, with their MIT licence.
- **Vulkan device.** Where the device has them, `shaderInt16`, `shaderStorageImageExtendedFormats`, `storageBuffer16BitAccess`, `shaderFloat16` and `subgroupSizeControl` are enabled (FidelityFX picks its shader variants from what the physical device supports), each from its own feature structure in the device's `pNext` chain; unused otherwise. FSR on Vulkan needs a 1.3 device. On Linux the same (unmeasured: no A/B could be run there). On the PS5 only when the game starts with FSR chosen (`temporal_aa_device_features`, off by default there, which `ps5/game/main_ps5.cpp` turns on from `mcla_anti_aliasing` before the device is made; the diagnostic stages before it keep the old device), so that everyone else's device stays as it was: chosen during play, FSR applies from the next start (`temporal_aa_available` is `next_start`, the plugin doesn't create FSR on a device without the features, the game uses MSAA meanwhile, and the menu says FSR 3.1 (NEXT START, UNTESTED) while that is so, FSR 3.1 (UNTESTED) when it can run, as on Linux). If FSR can't run after a start with it chosen (it failed, or isn't in the build), the setting becomes MSAA, which the menu shows anyway, saved with the next settings save, so that the console doesn't keep starting with FSR's features for nothing.
- **Settings.** `temporal_aa` (`off`, the default, or `fsr`; the game sets it from `mcla_anti_aliasing`), `temporal_aa_sharpness` (FSR's RCAS, 0 to 1, 0.2 by default); development: `temporal_aa_debug` (1 FSR's debug view, 2 no jitter, 3 jitter without the anti-aliasing, 4 a log every 120 frames with the jitter gate's counts, how far the pass got and its GPU time, 5 the reactive mask in red over the scene, 6 the motion vectors as colour), `temporal_aa_reactive`, `temporal_aa_flip_jitter`, `temporal_aa_cut_distance`. All change during play.

Checked on Windows, RTX 5080, at 3x unless noted, both APIs:

- **It runs:** FSR made in 619 ms the first time on D3D12 (585 ms on Vulkan), 10 to 30 ms after that; in the city 1,356 of 1,357 scene draws jittered (the one left is the fog quad), in the race all but one of 2,450 to 3,500. Switched on during play (from MSAA) and on from the start, the same view looks the same; with MOTION BLUR off and with FULLSCREEN BLUR off the pass runs as before.
- **The jitter's direction:** with the game's time stopped (`mcla_freeze`), the converged picture is sharpest with the jitter FSR is told as it is, against x, y or both flipped (`temporal_aa_flip_jitter`): mean |Laplacian| 7.93 against 7.47, 7.12 and 6.39 on D3D12, 6.92 against 6.20, 6.03 and 5.49 on Vulkan.
- **The stencil:** the motion blur's stencil view is swizzled ZYXW, so the vehicle ID is read from its third component (the pass takes it from the fetch constant's swizzle).
- **GPU time** of the vector pass, FSR with sharpening and the copy back (`temporal_aa_debug 4`, means of 120 frames): D3D12 in the city 0.16 ms at 1x, 0.58 ms at 2x, 1.38 ms at 3x, in the race at 3x 1.39 to 1.45 ms; Vulkan in the city at 3x 1.45 ms.
- **Validation,** at 1x through the night race with FSR, the reactive mask and both debug views on and switches to MSAA, off and back: the Vulkan validation layer with synchronization validation (`VK_LAYER_VALIDATE_SYNC=1`, `VK_LAYER_SYNCVAL_SHADER_ACCESSES_HEURISTIC=1`, `VK_LAYER_ENABLE_MESSAGE_LIMIT=0`) reported only what it reports without FSR: write-after-write hazards between the SDK's own compute dispatches into two buffers made at start-up (the texture loads' scratch buffer and the EDRAM buffer, 56,839 in the kept part of the log, the kinds `launcher/dev/single-tile-edram-2026-10-16.md` lists), `ShaderOutputNotConsumed`, and FidelityFX's own `Undefined-Value-StorageImage-FormatMismatch-ImageView` warning about its internal luma history; nothing on the pass's or FidelityFX's resources, through FSR on, off for over 10 seconds (released) and on again (the logs rotated, so the first minute, the boot, wasn't kept). The D3D12 debug layer (`--d3d12_debug=true`, bindless) over the same: no errors or corruption; only the usual warnings (initial states ignored for buffers, MIN/MAX blend factors, live objects at exit).
- **With it off:** no draw is jittered and the pass doesn't run (`temporal_aa_debug 4`: 0 scene draws seen as such), and no FidelityFX DLL is loaded. In the code: every change to existing SDK files is an addition; with `temporal_aa` off, `TaaCheckDraw` returns at once, `kSceneBegin` doesn't arm the jitter, the snapshots aren't taken, and no state, constant or pipeline key the draws use is touched. What runs always is a cvar read per frame, the markers' NOP packets, the availability check (a file test) once, and on Vulkan the device features above. The start-up screens are byte-identical to the build without the patch as often as two runs of that build are with each other (D3D12 21 of 40 shots, Vulkan 30 of 40, all of them the dark legal and logo screens). 3D frames can't be compared byte for byte: even with the game's time stopped, 12 consecutive frames are all different (they vary in the shadows from frame to frame), so they were not.
- **Against the previous build** (d3427bc), at 3x, builds alternating, medians of the runs' medians, every run clean by the GPU guard: Vulkan with MSAA, the race 10.13 against 10.14 ms (-0.1%, 3 pairs), the city drive 8.34 against 8.43 (-1.1%) and free roam 8.35 against 8.43 (-1.1%, 2 pairs); Vulkan with anti-aliasing off, the race 9.87 against 9.96 ms (-0.9%, 3 pairs); D3D12 with MSAA, the race 8.51 against 8.56 ms (-0.6%, 2 pairs). All within run-to-run noise (the short segments, the race's intro and the title, move by 4 to 8% either way from batch to batch).
- **Updating:** the launcher's Update (`build-game.ps1 -Update`, as the launcher runs it) in a fresh clone built at the previous release (d3427bc), from this commit: 2 min 43 s on this PC, fetching the FidelityFX SDK and building both runtimes included, and the game ran FSR on both APIs; the same with GitHub unreachable (a proxy that refuses connections): 1 min 32 s, a working game without FSR ("FSR is unavailable here; MSAA is used instead" when asked for it) and `make_pc.ps1`'s note saying so; and the next build with the network back built FSR (1 min 20 s).

- **The prebuilt shaders, checked on Windows** (`REXGLUE_FSR_PREBUILT_SHADERS=ON`, the Vulkan runtime's build folder afresh, as on Linux and the PS5): its `build.ninja` runs the stand-in 72 times and FidelityFX_SC.exe never, and the runtime it builds embeds 40 SPIR-V modules (the normal one 350, with FSR 2 and every permutation), all 40 byte for byte the same as in the normal runtime. FSR on Vulkan at 3x with it and then with the normal runtime, the same build otherwise: in the city by day with the game's time stopped the pictures are alike (mean |Laplacian| 8.76 against 8.73, the same brightness), the night race is clean in both, and the pass's GPU time is the same within noise (medians of the 120-frame means: city 1.48 against 1.50 ms, race 1.49 against 1.53 ms). Every run was clean by the GPU guard.
- **The PS5's static runtime, on this PC** (`launcher/dev/fsr/static_test`, how to run it at the top of its CMakeLists.txt): FidelityFX built by the SDK's own wrapper as static libraries (`REXGLUE_FSR_STATIC`, the prebuilt shaders, the SDK's Vulkan headers, no Vulkan library, clang 20 with `-flto=thin`; `-mcmodel=large` isn't available for this Windows target), and a program that links only those, the plugin's `temporal_aa_static.cpp` and its own code, with no Vulkan loader library and `/INCLUDE:ffxCreateContext` as the title's `--undefined`. It opens `vulkan-1.dll` at run time, fills the plugin's Vulkan function tables as the SDK does, hands them over as the plugin does, creates FSR with `kTaaFsrCreateFlags` on a device with FSR's features and the plugin's `vkGetDeviceProcAddr`, and dispatches it for eight frames of a flat picture: on the RTX 5080 creation, dispatch and destruction succeed and the output is the picture (0.4988, 0.2494, 0.1247 for 0.5, 0.25, 0.125). It catches both faults an independent review found in the first version: built without its folder's definitions the archive has no backend (`check_static.cmake` stops the build), and without `vkGetDeviceProcAddr` the program doesn't link.
- **Linux and the PS5** themselves can't be built or run here. Checked instead: the prebuilt path (the same CMake and stand-in Linux and the PS5 use), the static runtime as above, clang syntax checks with the Windows build's command lines of the code only they compile (`REX_FSR_STATIC` in `temporal_aa.cpp` and `vulkan/temporal_aa.cpp`, `vulkan/temporal_aa_static.cpp`, the prebuilt flags check), and reading. Not checked: FidelityFX's sources compiling with the PS5's toolchain and libraries, the title's link with them (`ps5/game/build.sh` falls back to linking without them), the forwarded Vulkan functions against the PS5's driver, FSR's device features on Linux drivers and on the PS5's RADV, a Linux build, and anything running on either.

Not done yet: the reactive mask on by default; building and running it on Linux and on a PS5. The shaders are built like the HDR ones (`launcher/dev/hdr/build_shaders.py`).

| Change | Files | Affects |
|---|---|---|
| Marker kinds, the jitter gate, the anti-aliasing point, the matrices, the frame state, availability, the options | `include/rex/graphics/command_processor.h`, `src/graphics/command_processor.cpp`, `src/graphics/temporal_aa.cpp`, `src/graphics/temporal_aa_fsr.h` | all platforms |
| The pass, the reactive mask, FSR's context, dispatch and release, the timers, the debug views | `src/graphics/d3d12/temporal_aa.cpp`, `src/graphics/vulkan/temporal_aa.cpp`, `src/graphics/{d3d12,vulkan}/command_processor.cpp` and headers | D3D12; Vulkan |
| The external callback command | `src/graphics/d3d12/deferred_command_list.cpp`, `include/rex/graphics/d3d12/deferred_command_list.h`, `src/graphics/vulkan/deferred_command_buffer.cpp`, `include/rex/graphics/vulkan/deferred_command_buffer.h` | D3D12; Vulkan |
| The bound textures' resources and states; reading a colour target (the snapshots) | `src/graphics/{d3d12,vulkan}/texture_cache.cpp`, `src/graphics/{d3d12,vulkan}/render_target_cache.cpp` and headers, `include/rex/graphics/pipeline/render_target/cache.h` | D3D12; Vulkan |
| Device features | `src/ui/vulkan/vulkan_device.cpp`, `include/rex/ui/vulkan/device.h` | Vulkan |
| The static runtime's Vulkan functions | `src/graphics/vulkan/temporal_aa_static.cpp` | PS5 |
| The shaders | `src/graphics/shaders/taa_motion_vectors_mcla.cs.xesl`, `taa_luma_snapshot.cs.xesl`, `taa_reactive_mcla.cs.xesl`, `taa_debug_view.cs.xesl`, their headers under `bytecode/d3d12_5_1` and `vulkan_spirv` | D3D12; Vulkan |
| Building FSR; the ffx-api headers and licence | `CMakeLists.txt`, `src/graphics/CMakeLists.txt`, `cmake/rexglue_fsr.cmake`, `cmake/fidelityfx/CMakeLists.txt`, `cmake/fidelityfx/soft_step.cmake`, `cmake/fidelityfx/prebuilt_shaders_hook.cmake`, `cmake/fidelityfx/prebuilt_shaders.cmake`, `cmake/fidelityfx/vulkan-headers-only/FindVulkan.cmake`, `thirdparty/fidelityfx-api/` | Windows; Linux and the PS5 (not built there yet) |

## rexglue-temporal-aa-shaders-vk.patch

**Why.** FSR's Vulkan runtime embeds its shaders as SPIR-V in headers that FidelityFX's shader compiler, FidelityFX_SC.exe, generates at build time; it is a Windows program, so Linux and the PS5 (whose build host is Linux) can't run it. This patch adds those headers, prebuilt, for the permutations rexglue's native anti-aliasing can ask for, so neither needs Wine or a Windows tool. Generated, so in a patch of its own; `rexglue-temporal-aa.patch` uses it.

**What it adds.** `thirdparty/fidelityfx-shaders-vk/`, made by `launcher/dev/fsr/fsr_vk_shaders.py` from a Windows build's FidelityFX_SC output at the pinned FidelityFX commit:
- For each of the FSR 3 upscaler's ten passes and its four device variants (wave32 or 64, FP16 or not; the device picks), the permutation table (`*_permutations.h`) with the keys rexglue's create flags reach (HDR input, inverted depth, vectors at the render size, no jitter cancellation: four of 64, the Lanczos table that the device picks times sharpening) leading to their blobs, unchanged, and every other key to an empty entry; tables FidelityFX's code doesn't use (the 16-bit ones of the passes without FP16, whose blobs its runtime doesn't embed) empty: 40 blobs, 0.94 MB of SPIR-V, 5.17 MB as headers (115 files).
- FSR 2's 32 tables empty (ffx-api links FSR 2, but rexglue asks for FSR 3.1).
- `rexglue_fsr_shaders_vk.h`: the create flags the set is for, of those that choose FSR 3's permutations; the GPU plugin compares its own with them before it creates FSR from this set.
- FidelityFX's MIT licence and a README.

The script stops without writing on a FidelityFX checkout that isn't the pinned commit or has other trees or modified files (as `rexglue_fsr.cmake` checks it), on an option it doesn't know, on create flags in the GPU plugin it can't map to options (`kTaaFsrCreateFlags` in `temporal_aa_fsr.h`, which both backends must use as they are), on FidelityFX choosing its permutations from the flags differently from what it assumes (the lines of `ffx_fsr3upscaler.cpp` it checks), on a table the accessor indexes with a key of another layout, on a table whose blobs the runtime embeds only in part, on a build that itself used the prebuilt set, and when its own output has a key rexglue can ask for that leads to an empty entry. Its output doesn't depend on the order FidelityFX_SC writes things in (it varies from run to run). Run it again after a change of the FidelityFX commit or of the create flags, and remake the patch.

| Change | Files | Affects |
|---|---|---|
| The prebuilt shaders, their flags, licence and README | `thirdparty/fidelityfx-shaders-vk/` | Linux, the PS5 (Windows only with `REXGLUE_FSR_PREBUILT_SHADERS=ON`) |

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

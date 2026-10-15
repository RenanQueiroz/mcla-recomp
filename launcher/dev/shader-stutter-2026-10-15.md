# Shader and pipeline stutter (2026-10-15)

Windows 11, RTX 5080, Ryzen 7 9800X3D, the launcher's settings.

## What there was

- **Storage:** the persistent shader storage works on both APIs with this fork's patches.
  - Every start preloads the stored pipelines on the pipeline creation threads (12 here) before the game runs: about 1,690 on D3D12 in 50 ms, about 1,030 on Vulkan in 0.35 s.
  - The storage is in `<user data>/cache/shaders/shareable`, about 0.5 MB.
  - devrun's runs use their own (`--cache_root`).
- **Warm sessions:** with the storage and the driver's own shader cache both warm, nothing is created during play. 90% of the earlier benchmark runs had no pipeline cache miss at all.
- **Cold or new content:** stalls come from a first session, new places, cars or races, and the first run after a driver update or after a change to the translated shaders.
  - `async_shader_compilation` (on by default) leaves a draw out while its pipeline is being created on the creation threads.
  - At the end of every submission, `PipelineCache::EndSubmission` on both APIs then created the rest of the queue on the GPU command thread and waited for every pending creation. The frame was held for the slowest compile, and still went out without those draws.
  - Vulkan also created a placeholder for each new pipeline (the real vertex shader with a stand-in pixel shader) on the GPU command thread first. The draw is left out anyway, and creating it compiles the vertex shader, which with a cold driver cache takes about as long as the real pipeline.
- **Example:** the 15-minute 3x soak in `d3d12-zoomout-2026-10-15.md` had one such frame. A never-seen pipeline (VS `4AFAE967AC1636BA`, PS `A38DDC43C85BA2B7`) started creating 9 ms into a 164 ms frame.

## Method

- **The driver-cache salt:** `shader-stutter-timers.sdk.patch` adds a dev-only setting, `tmp_shader_salt`.
  - It makes the translated shaders differ for the driver's cache without changing what they do: the DXBC generator string, and an unused SPIR-V constant (an `OpSource` version alone did not get past NVIDIA's Vulkan cache).
  - So a run sees the driver's cache cold without the NVIDIA cache being emptied; it is shared with other games.
  - `__GL_SHADER_DISK_CACHE=0` has no effect on NVIDIA's Windows driver.
- **Timers:** the same patch logs, as `TMPSS` lines:
  - every pipeline created, with its translation and creation time;
  - per frame, the draws left out, the time waited within the budget, the end-of-submission wait, the pipeline work done on the GPU command thread and the presents skipped;
  - the Vulkan preload's translation and creation times.
- **Scenarios**, each run with a storage folder of its own (`devrun.py --cache-root`), deleted afterwards:
  - **cold:** empty storage and a new salt, as on a first run;
  - **post-update:** a copy of the dev storage (1,693 D3D12 and 1,030 Vulkan pipelines) and a new salt, as after a driver update or a change to the translated shaders;
  - **warm:** the same copy without a salt.
- **Script:** `race.pad` under `--mcla_fixed_dt=60`, uncapped, windowed: the title, the map zoom-out, the menus into the arcade race, its intro and 90 s of it.
- **Comparison:** "off" is the behavior before these patches:
  - `--async_pipeline_end_of_frame_wait=true --async_pipeline_wait_budget_ms=0`
  - `--vulkan_async_skip_incomplete_frames_max_ms=0 --vulkan_async_placeholder_pipelines=true`
  - `--vulkan_pipeline_storage_translation_threads=1`
- **Runs:** two per cold and post-update configuration, one per warm one, on and off alternating.
- Frames are counted from the title on.

## Results

| Scenario | API | Res | Fixes | Frames >50 ms | >100 ms | Worst | Time in frames >50 ms | Zoom-out 1% low | Race 1% low |
|---|---|---|---|---|---|---|---|---|---|
| cold | D3D12 | 1x | off | 7.0 | 1.5 | 326 ms | 0.76 s | 18.6 FPS | 73.9 FPS |
| cold | D3D12 | 1x | on | 3.5 | 2.0 | 159 ms | 0.38 s | 26.1 FPS | 79.9 FPS |
| cold | D3D12 | 3x | off | 10.0 | 2.0 | 398 ms | 1.02 s | 13.4 FPS | 56.7 FPS |
| cold | D3D12 | 3x | on | 5.0 | 2.0 | 190 ms | 0.52 s | 23.0 FPS | 64.8 FPS |
| cold | Vulkan | 1x | off | 83.0 | 57.5 | 1,850 ms | 17.35 s | 2.5 FPS | 13.4 FPS |
| cold | Vulkan | 1x | on | 3.0 | 2.0 | 173 ms | 0.37 s | 19.4 FPS | 55.1 FPS |
| cold | Vulkan | 3x | off | 89.0 | 57.5 | 1,919 ms | 17.56 s | 2.4 FPS | 15.2 FPS |
| cold | Vulkan | 3x | on | 3.5 | 1.5 | 246 ms | 0.43 s | 22.1 FPS | 50.8 FPS |
| post-update | D3D12 | 1x | off / on | 2.5 / 2.5 | 1.0 / 1.0 | 115 / 111 ms | 0.24 / 0.25 s | 32.1 / 32.1 FPS | 89.2 / 86.4 FPS |
| post-update | D3D12 | 3x | off / on | 4.0 / 3.0 | 1.0 / 1.0 | 169 / 166 ms | 0.37 / 0.32 s | 24.8 / 27.3 FPS | 72.5 / 71.3 FPS |
| post-update | Vulkan | 1x | off / on | 2.5 / 2.5 | 0.5 / 0.0 | 106 / 98 ms | 0.20 / 0.20 s | 34.1 / 32.4 FPS | 69.1 / 70.9 FPS |
| post-update | Vulkan | 3x | off / on | 2.5 / 3.0 | 0.5 / 0.5 | 106 / 104 ms | 0.22 / 0.25 s | 29.8 / 28.1 FPS | 66.0 / 64.4 FPS |
| warm | D3D12 | 1x | off / on | 2 / 4 | 1 / 2 | 113 / 119 ms | 0.20 / 0.35 s | 31.5 / 38.6 FPS | 92.2 / 69.5 FPS |
| warm | D3D12 | 3x | off / on | 2 / 2 | 1 / 2 | 185 / 174 ms | 0.28 / 0.28 s | 28.3 / 27.7 FPS | 72.2 / 70.6 FPS |
| warm | Vulkan | 1x | off / on | 2 / 2 | 0 / 0 | 94 / 97 ms | 0.17 / 0.17 s | 38.7 / 34.2 FPS | 69.8 / 70.6 FPS |
| warm | Vulkan | 3x | off / on | 2 / 3 | 1 / 0 | 120 / 95 ms | 0.21 / 0.24 s | 31.1 / 27.3 FPS | 62.2 / 62.7 FPS |

(The cold Vulkan "on" rows are from the final build, with depth-only pipelines created asynchronously too; the earlier build without that had 3.0 to 4.0 frames over 100 ms and a worst of 273 to 285 ms.)

- **Cold sessions:**
  - **Pipelines:** about 1,000 created on D3D12 and 760 to 780 on Vulkan.
  - **Creation times:** D3D12 cold compiles take up to 80 to 100 ms each. Vulkan ones take 13 to 18 ms median and up to 465 ms.
  - **Before:**
    - On D3D12 the frame end waited 1.3 to 1.5 s in total per session, up to 174 ms in one frame.
    - On Vulkan, 11.7 to 12.7 s, plus 4.3 to 4.5 s of placeholders on the GPU command thread (one frame held 1.5 to 1.6 s).
  - **After:**
    - The frame end waits 0.
    - What is left on the GPU command thread is D3D12's depth-only pipelines and their shaders (70 to 200 ms a session, up to 21 to 56 ms in a frame) and Vulkan's shader translations (about 420 ms a session, up to 82 to 136 ms in a frame).
    - The frame budget waited 0.7 to 0.8 s in total on D3D12 and 2.6 to 3.0 s on Vulkan (4 ms a frame at most). It drew about 370 to 430 (D3D12) and 240 to 290 (Vulkan) draws that would otherwise have been left out.
- **Post-update and warm sessions:** the storage covers the whole run, so nothing is created during play and the fixes change nothing there.
  - The frames over 100 ms in those rows are the same ones with or without the fixes: one at the title screen (swap 693 in every run, 100 to 185 ms with nothing created) and the race's loading.
  - The warm D3D12 1x "on" run's lower race 1% low is a single run with no pipeline activity at all.
  - The cost is at start-up: the preload compiles the stored pipelines with the driver's cache cold, 0.85 to 0.95 s on D3D12 and 5.0 s on Vulkan, once, before the logos.
- **Race median while compiling:** a cold Vulkan session's race median rises from 8.2 to 8.8 ms (1x), as the creation threads keep compiling in the background during play instead of in held frames.

## What shows: pop-in

- **D3D12:** draws left out in a cold session add up to 15,000 to 17,500 over 130 to 145 frames, the same kind as before (13,000 to 16,000 over 190 to 230 frames). Before, those frames were also held.
- **Vulkan:** 220,000 to 490,000 draws over 530 to 620 frames.
  - With `vulkan_async_skip_incomplete_frames`, 205 to 250 of those presents were skipped (at most 100 ms in a row, then the incomplete frames show).
  - Before, the presents were skipped too (140 to 150) and the frames held.
- **Screenshots** (real time, the dev pad's `shot` through the zoom-out and into the city):
  - Cold, D3D12, on: nothing missing in the shots.
  - Cold, Vulkan, on: one shot 5.6 s into the zoom-out has some buildings missing (grey where they'd be) for that moment.
  - Cold, Vulkan, off: the zoom-out was still high above the city when the others were at street level. It is a timed loop, and the held frames put it 1 to 2 s behind.
  - A first session on Vulkan therefore shows buildings popping in during the zoom-out instead of it freezing for up to 1.9 s.

## Not kept

- **Lower priority for the creation threads** (`tmp_creation_priority` in the timer patch): no clear change in cold D3D12 runs.
- **A VkPipelineCache file** (`rexglue-vulkan-pipeline-cache-file.patch`) is off by default:
  - **Cold start-up:** with a cold driver cache, the preload of 1,030 pipelines took 5.0 s with or without it.
  - **Warm start-up:** with the file and the driver's cache warm, 21 against 25 ms.
  - **Missing from the file:** in a run whose pipelines the driver had cached but the file did not, creating them took about ten times as long (median 1.5 against 0.16 ms).
  - **Size:** the file is 19 MB for 1,030 pipelines.
  - NVIDIA's driver has its own disk cache; the file is for drivers that don't.
- **Vulkan's shader translation on the creation threads** (as D3D12's async path does): the pipeline layout depends on the translated shader's bindings and comes from the command processor's layout cache, so it would need more restructuring. It is the main part of what is left on Vulkan's GPU command thread in a cold session.

## Tools

- `shader-stutter-timers.sdk.patch` (SDK, `TMPSS`): the salt and the timers above. It applies on top of the three patches.
- `devrun.py --cache-root <folder>` (a storage folder for the run) and `--env NAME=VALUE`. devrun now also checks that the user's shader cache stays unchanged.

## Checks

- **Warm benchmark:** `drive-spawn.pad`, timedemo, fullscreen, five runs a side, alternating after a warm-up. Median frame, with the 1% low in brackets:

| Segment | API | Res | Before | After |
|---|---|---|---|---|
| Map zoom-out | D3D12 | 1x | 6.87 ms (33) | 6.88 ms (31) |
| Map zoom-out | D3D12 | 3x | 8.08 ms (28) | 8.03 ms (28) |
| Map zoom-out | Vulkan | 1x | 6.93 ms (34) | 6.95 ms (33) |
| Map zoom-out | Vulkan | 3x | 8.37 ms (31) | 8.33 ms (31) |
| Free roam | D3D12 | 1x | 6.87 ms (116) | 6.88 ms (117) |
| Free roam | D3D12 | 3x | 10.12 ms (79) | 10.20 ms (81) |
| Free roam | Vulkan | 1x | 6.86 ms (118) | 6.82 ms (120) |
| Free roam | Vulkan | 3x | 9.29 ms (94) | 9.35 ms (86) |
| Drive | D3D12 | 1x | 6.83 ms (114) | 6.83 ms (116) |
| Drive | D3D12 | 3x | 9.65 ms (82) | 9.77 ms (81) |
| Drive | Vulkan | 1x | 6.86 ms (118) | 6.83 ms (117) |
| Drive | Vulkan | 3x | 9.45 ms (89) | 9.43 ms (87) |

- **60 FPS cap, native, free roam:** frames within 1 ms of the target:
  - D3D12: 99.3 and 99.2% after, 100.0 and 98.5% before;
  - Vulkan: 98.5 and 98.3% after, 98.8 and 99.2% before;
  - worst frame 17.6 to 18.2 ms throughout.
- **D3D12 debug layer**, a cold 3x race run: no errors, only the creation warnings that were there before, and the list of live objects at exit.
- **Vulkan validation**, a cold 3x race run: only `ShaderOutputNotConsumed` and `vkCmdEndQuery` 01923, both there before.
- **An early exit:** one of the 48 benchmark runs, D3D12 at 3x with the old behavior, ended 3 s into the map zoom-out with nothing in the log (no crash report, no dump). It had created no pipelines. The same configuration ran three more times without it.

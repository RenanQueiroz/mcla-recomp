# Known issues and findings

Updated 2026-10-03. PC (Windows, D3D12 and Vulkan). On PS5 only the memory probe has run; see `ps5-feasibility.md`.

## Fixed: cars white with a glow halo (the car reflection bug)

2026-10-12. The light glows in the cars' environment map are blended with MAX and a SrcAlpha source factor, which the Xbox 360's GPU applies and Direct3D 12 and Vulkan ignore; each glow became a solid square and the paint mirrored it. Fixed in `patches/rexglue-minmax-blend-factor.patch` for both backends. Confirmed in game on Windows/D3D12 and on a PS5 (Vulkan); on Windows/Vulkan it builds and runs. Full account: `../REFLECTION_FIX.md`.

## Milestone reached

**Boots to the title screen** on Windows/D3D12 (2026-10-03, run 20261003-103007): the Complete Edition logo over the live 3D city with traffic, "PRESS START" prompt, about 38 s after launch. The process was still running at 75 s with 77 threads and 1.36 GB working set, and the log had no errors other than the missing audio device.

**Menus, races and the garage work** on Windows/D3D12 (2026-10-03, user play session with an Xbox 360 controller, about 20 minutes across five launches). The user reported: gameplay very good, audio fine, got into the garage, installed hydraulics and they worked. Four crashes on unrecovered functions, each fixed by the recovery loop and not seen again.

Problems the user reported in that session:

- **Frame-rate drops on loading screens.**
- **Random "slow motion" during races.** The game runs a fixed timestep, so any stretch below 30 FPS plays slow. Cause not yet established. Candidates: the SDK vblank-timer underflow that LARecomp documented (fixed here in `patches/rexglue-vblank-resync.patch`, effect unverified), pipeline/shader compilation hitches, and write-watch faults.

To find out, Release builds now keep the SDK's per-frame counters (`patches/rexglue-perf-csv-env.patch`). Set `REX_PERF_LOG_CSV=<file>` before launching and run `scripts/analyze_perf.py <file>` afterwards.

## Stuck on one shot of the city when an event starts (issue #6)

Reported entering a Red Light race on PC: the camera holds one shot, people walk and music plays, and no input does anything. The likely cause is the game's warper, which moves the player to an event or the garage behind a camera flight. It notices the end of the flight by predicting it one frame early from a duration measured at a fixed 1/30 s; with real frame times it can miss that frame and then wait for good. LARecomp found this with the garage picked from far away and measured it.

`src/warp_fix.cpp` (ported from LARecomp's `warper.cpp`) watches the warper after each update and, when a flight has ended without the game noticing, finishes the warp the way the game would have, logging `warp fix: ... finishing the warp`. On by default (`mcla_warp_fix`). `mcla_warp_test_miss` makes every flight end that way, to check the fix.

Checked on PC (2026-10-14): builds, and a garage trip close to the garage still works (it used no flight). Not checked: the hang itself, which needs a far-away event or garage. Whether it is the cause of issue #6 is not confirmed.

## Boot blockers found so far

| # | Symptom | Cause | Status |
|---|---|---|---|
| 1 | `Validation failed: N unresolved calls` at codegen | Tail-call targets not inside any discovered function | Fixed with 8 hints in `config/mcla.toml` |
| 2 | `Call to invalid or unregistered function at guest address ...` at run time | Functions reached only through pointers | Ongoing. 30 found by data scan (`config/data_referenced.toml`), 46 by a scan for pointers built in code (`config/code_referenced.toml`, 2026-10-12, after a stop on accepting a delivery mission), the rest one per run (`config/runtime_discovered.toml`, `scripts/recover_loop.ps1`) |
| 3 | `VFS: 't:\mc4\art\city' -> [no device]` | Game probes a `t:` drive for loose city files | Fixed: `t:` is mapped to the game root in `MclaApp::OnPostSetup`. The probes now fail with "not found", which the game tolerates |
| 4 | Guest null-pointer read at 0x00002EF0 in `sub_821C8FE0` | See below | Fixed with the silent audio driver in `src/audio_fallback.cpp` |

### Issue 4 in detail

The game's audio engine init (`sub_82144B90`) calls the statically linked XAudio library (`sub_823EAA88`), which registers a render-driver client with the kernel. If the host cannot create an audio driver, that returns 0xC0000002 or a failure code, `sub_82144B90` returns false, the caller (`sub_82144EB0`) skips creating the engine's handle table (pointer at guest 0x828369C4), and the first user of that table dereferences null.

Two host conditions trigger it:

- SDL cannot open an output device (`No default audio device available`). This PC is in that state.
- The SDK's `NopAudioSystem`, whose `CreateDriver` returns `X_STATUS_NOT_IMPLEMENTED`.

So the game needs an audio driver to exist even when there is nowhere to play sound. `SilentAudioDriver` accepts frames, discards them, and releases the client semaphore once per 256-sample frame period (5.33 ms) so the guest runs at real-time pace. This matters for PS5 bring-up too: the game can boot before the PS5 audio output is written.

Useful addresses learned along the way:

| Address | What |
|---|---|
| 0x82144B90 | Audio engine init, nine sequential checks |
| 0x82135E48 | Loads `$/audio/X360/Config/driverSettings.xml`, then sets up voice pools |
| 0x8213AB78 | Voice pool setup: 100 voices x 8320 bytes, 2048-byte alignment |
| 0x823EAA88 | XAudio library init (XAUD static lib) |
| 0x82160800 | Creates the 800-slot handle table |

## Open risks, not yet investigated

- 20 `Unexpected float16_4 pack instruction` codegen warnings in 0x8243C67C to 0x8243D158.
- Vulkan reaches the same title screen (run `20261003-103756`), but only with the default feature set on an RTX 4080. The minimal feature set is not established.
- Audio output itself is untested because this PC has no default output device.
- Only boot and the title screen have been seen. Menus, loading a race and gameplay are untested (they need controller or keyboard input).
- Frame rate and frame pacing have not been measured.

## Tooling notes

- `--mcla_scan_code_pointers=<file>` scans the loaded image for data words pointing at unrecovered function entries and exits.
- Guest crash tracer (`src/crash_trace.cpp`): on a null-page fault, logs the host stack with each frame mapped to its guest function. Windows only for now; needs a `sigaction` version for Linux and PS5.
- Git on Windows checks out the SDK's libmspack symlinks as text files. They must be replaced with copies of their targets before building the SDK.
- lldb from the portable LLVM package does not start (needs `python310.dll`).

## Performance: second play session (2026-10-03, run `play-20261003-153451`)

18.5 minutes in one launch, D3D12, with the vblank resync patch and per-frame counters. No crashes. User report: two small hitches, much better than the first session, frame-rate drops still present on loading screens.

| Measure | Value |
|---|---|
| Frames | 32,603 |
| Frame time | median 33.3 ms, p95 36.1 ms, p99 50.2 ms, max 437.5 ms |
| Frames over 40 ms | 547 (1.7%), 37 s in total (3% of the session) |
| Longest slow stretch | 4.0 s |
| Interrupts per frame | about 5 normally, 6 in slow frames: no interrupt storm |
| Pipeline cache misses | 0 in both normal and slow frames |

What the slow frames have in common: **texture cache misses**. 510 of the 547 slow frames (35 of the 37 slow seconds) had at least one miss; the median slow frame had 38, the median normal frame 0. Misses are steady through the whole session at 2,500 to 5,400 per minute, 63,484 in total. Most frames with misses are still on time (8,658 frames had misses, 510 of those were slow), so it is the bursts that hurt.

Not established:

- Whether the misses are first-time loads (streaming new areas) or reloads after eviction. The steady rate over 18 minutes in one city suggests eviction. LARecomp reports that raising the cache limits to 1536 MB soft / 2048 MB hard / 64 MB render-to-texture took misses to zero. `scripts/play_loop.ps1 -BigTextureCache` applies those values; not yet tested here.
- Whether the improvement over the first session came from the vblank patch. The first session had no counters, so there is no before/after measurement. No storm occurred in this session, which is consistent with the patch working but does not prove it.
- Which slow stretches were loading screens. The counters carry no game-state marker.

PS5 note: a 2 GB texture cache is a memory-budget question there, and the budget is still unmeasured.

## Performance: third play session, `-BigTextureCache` (2026-10-03, run `play-20261003-155942`)

5.4 minutes. User report: a little slow motion, about the same as before.

| Measure | Session 2 (default cache) | Session 3 (1536/2048 MB cache) |
|---|---|---|
| Texture cache misses per minute | 3,445 | 4,176 |
| Frames over 40 ms | 1.7% | 6.2% |
| Slow frames that had texture misses | 510 of 547 | 369 of 580 |
| Frames with more than 5,500 draw calls that were slow | 6% | 30% |

Conclusions:

- **The larger texture cache did not reduce misses**, so the misses are not evictions. They are most likely first-time loads as the open world streams in. The option is not worth its memory and should stay at the default, which also suits the PS5. Not verified: that the three options were actually applied (the log does not echo them).
- **This session is not a clean measurement.** A four-job compile was running in WSL on the same machine for the whole session. The jump in slow heavy-scene frames (6% to 30%) is most likely that competing load, which itself shows the game is CPU-bound in heavy scenes.
- **Slow frames cluster at 50 ms.** With vsync on, a frame that misses the 33.3 ms deadline waits for the next display interval and becomes 50 ms (20 FPS), so a small overrun costs a third of the speed. The median normal frame is already at 33.3 ms with a 95th percentile of 36 ms, so there is little headroom. LARecomp's notes describe the same quantisation and solve it by disabling vsync and pacing frames with a host-side limiter; that needs a frame limiter here, because without one the game runs faster than real time.

## Frame limiter and frame-rate targets (2026-10-03)

`src/frame_timing.cpp` and `config/frame_timing.toml`. The game's timer now receives the real elapsed time instead of a fixed 33.3 ms step, vsync is off by default, and a host limiter sleeps to a wall-clock deadline. `--mcla_fps=30|60|120` selects the target (default 30), `0` is uncapped, `-1` restores the original fixed-step behaviour. Hook addresses and timer analysis are from LARecomp's notes.

Title screen, RTX 4080 and Ryzen 7 7800X3D, D3D12, last 15 seconds of each 60-second run:

| Target | Measured | Median frame | 5th to 95th percentile | Draw calls per frame at that moment |
|---|---|---|---|---|
| 30 | 30.0 FPS | 33.40 ms | 29.6 to 37.1 ms | 6,736 |
| 60 | 60.0 FPS | 16.58 ms | 13.9 to 19.7 ms | 3,873 |
| 90 | 67.5 FPS | 14.55 ms | 13.1 to 17.3 ms | 3,683 |
| 120 | 37.4 FPS | 26.45 ms | 24.4 to 29.8 ms | 7,706 |
| uncapped | 51.3 FPS | 19.45 ms | 17.3 to 22.2 ms | 4,970 |

Reading this:

- **The limiter holds 30 and 60 exactly** when the scene allows it.
- **The game is CPU-bound well below 120 FPS.** The title screen's camera flies over the city, so each run ended on a different view; frame time tracks draw calls at roughly 3.5 to 4 microseconds per draw. The 120 run was not slower because of its target, it ended on a heavier view (7,706 draws). The ceiling on this PC is about 50 to 70 FPS on the title screen.
- **Two threads are saturated** when uncapped (`scripts/thread_profile.ps1`): the guest thread that builds the frame (95% of a core) and the SDK's "GPU Commands" thread that translates Xenos commands to the host API (94%). Everything else is under 10%. Raising the frame rate means speeding up both.
- At 30 FPS a 6,700-draw frame needs about 23 to 26 ms of the 33.3 ms budget, which is why busier gameplay scenes overrun.

Not yet verified:

- Gameplay at any target. Only the title screen was measured. Whether real-delta timing removes the slow motion in races needs a play session.
- Correct game speed at 60. LARecomp reports the simulation is rate-independent with these hooks but needed further fixes for camera smoothing, chassis roll and ground filtering above 30 FPS; none of those are ported here. Intro movies are reported to play fast.
- Frame-to-frame jitter at 30 is about plus or minus 4 ms. The limiter waits inside the timer update, not at present, which may be the cause.
- That `vsync=false` set from the app reaches the GPU plugin. The log reports the value as false and the 60 FPS result is consistent with it.

## Frame-rate correctness fixes (2026-10-03)

User report at `--mcla_fps=60` before these fixes: intro movies play fast; camera smoothing, chassis roll and ground filtering are wrong, as LARecomp's notes predicted.

Ported from LARecomp into `config/frame_timing.toml` and `src/frame_timing.cpp`:

| Hook | Address | What it corrects |
|---|---|---|
| `mcla_camera_pos_smoothing` | 0x82320468 | Chase camera position filter factor, rewritten as a function of elapsed time |
| `mcla_camera_lookat_smoothing` | 0x823204F4 | Chase camera look-at filter factor, same treatment |
| `mcla_chassis_depth_smoothing` | 0x82563720 | Chassis ground-depth filter (0.10 per frame at 30 FPS) |
| `mcla_hook_skip_intro` | 0x822C2F08 | Skips the intro movies (`--mcla_skip_intro`, default on) |
| `mcla_skip_intro_render_pass_mask` | 0x821315E4 | Keeps the skip from enabling an unprepared render pass |

The conversion used for each filter: a per-frame fraction k tuned at 30 FPS becomes `1 - (1 - k)^(dt * 30)` for a step of dt seconds.

Status: builds and boots at the 60 target with no crash. **Not verified by eye**; needs a play session. With the intro skip on, three draws fail once at the moment of the skip (`Resolve region is empty`, six log lines at one timestamp); it does not recur. With the skip off there are none.

## Profile and rendering performance fixes (2026-10-03)

Sampling profile of the title screen, uncapped, before any fix (`--mcla_profile`, `scripts/profile_report.py`, symbol-enabled build):

Game thread (96% busy):

| Share | Where | What it is |
|---|---|---|
| 35.8% | `sub_82412F98` | Fence poll: 32 no-op spin, waiting on the GPU thread |
| 16.4% | `sub_82411E98` | `D3DDevice_BlockOnFence`, the loop around it |
| 13.9% | `sub_823D924C`, `sub_823D91FC` | Register restore/save helpers, called from that loop |
| 2.9% | `sub_8244FEC8` | The game's sleep |

About two thirds of the game thread was waiting. The bottleneck is the SDK's "GPU Commands" thread (90% busy): 62% in the SDK's Xenos translation, 14% in the NVIDIA driver, 10% in ntdll, 7% in memcpy. Within the SDK the largest items were register writes (`GetRegisterInfo` 8.7%, `WriteRegister` 8.5% + 5.5%, `ExecutePacketType0` 3.9%) and `UpdateBindings` 7.3%.

Fixes made:

| Fix | Where | Source |
|---|---|---|
| Single-tile rendering of the main scene, with the EDRAM limit raised to the host's 4096 tiles | `config/render_perf.toml`, `src/render_perf.cpp`, `--mcla_single_tile` | LARecomp |
| Yield in the fence poll instead of spinning | same, `--mcla_fence_yield` | LARecomp |
| Debug-only `GetRegisterInfo` lookup removed from every register write in Release builds | `patches/rexglue-register-write-fastpath.patch` | this project |

Title screen, uncapped, last 15 s of a 60 s run, same build with switches:

| Configuration | FPS | Median frame | Draw calls per frame |
|---|---|---|---|
| Both hooks off | 38.3 | 25.7 ms | 7,967 |
| Fence yield only | 46.3 | 21.5 ms | 6,803 |
| Fence yield and single tile | 64.7 | 15.3 ms | 4,927 |

All three include the register-write patch. Caveat: the title camera moves, and its path depends on elapsed game time, so the three runs are at the same time into the run but not guaranteed to be the identical view. The draw-call drop with single tile is the expected effect of submitting the scene once. A screenshot with both on shows the title screen rendering correctly, with no errors logged. Not yet checked in gameplay or with MSAA-sensitive scenes.

## Intro skip removes the title logo (2026-10-03)

Reported by the user, reproduced with window captures: with `--mcla_skip_intro` the title screen shows only "PRESS START", with or without the render-pass mask change. The skip is now off by default. Consequence: the intro movies play at the limiter's rate, so they are fast at targets above 30. A proper fix is to hold the limiter at 30 while a movie plays; that needs a way to detect movie playback.

Also visible in captures: a dither pattern on foliage (palm fronds, tree shadows). LARecomp lists the same artifact as unresolved.

Screenshots now use `PrintWindow`, which captures only the game window.

## Intro sequence ran too fast (fixed 2026-10-04)

The "intro movies" are not Bink video: they are the legal screens and the publisher logos, drawn in a loop that never calls the engine timer (sub_821BDA90), so the frame limiter there never ran. On the console that loop was paced only by the present interval of two vblanks, which `mcla_present_interval` removes; the sequence was over in about a second on PC and ran at 60-90 presents/s on PS5. Now a swap that follows another with no pass through the limiter in between is held to 30 per second (`PaceUnlimitedSwap` in `src/frame_timing.cpp`). Seen on PC by window captures (legal screens about 10 s, logos after) and on PS5 by the user.

`mcla_intro` = `normal` (default), `fast` (no such hold; the title screen keeps its logo) or `skip` (the old `mcla_skip_intro`, which loses the logo); INTRO under CONTROLS in the in-game menu. On PS5 the menu's settings are saved to and read from `/data/mcla/mcla.toml` (the host's own settings are applied after and win); menu, save and load confirmed there by log and by the user.

Not checked: whether the real Bink movies (`intro720.bik`, `attract720.bik`) play at the right speed. The game's player (sub_82468800) asks BinkWait (sub_827BFB58), whose clock is the kernel tick count; no Bink movie plays during start-up.

## Frame pacing: the limiter waits at the swap (2026-10-14)

The open point above, "the limiter waits inside the timer update, not at present", was the cause of uneven frames at a cap. The step was measured before the wait and the frame reached the screen after a variable amount of work, so at a 60 cap frames alternated around 12 and 22 ms and moving objects shook. `mcla_pace_at_swap` (default on) moves the wait to the swap (`mcla_present_interval`): frames are held to the target where they are presented, as vsync does but with a precise clock, and each step measures one presented interval. `--mcla_pace_at_swap=false` restores the old wait.

Windows, D3D12, RTX 5080 and Ryzen 7 9800X3D: on the title screen at 60 the frame-time standard deviation went from 3.1 ms to 0.38 ms. PresentMon at 45, 50 and 60 caps: median exactly on target, about 98% of presents within 1 ms of it, no dropped frames. In play the user reported it smooth at 60 and 120.

Measured with `--vsync=false` on the command line. The other open point above does not hold: `vsync=false` set from the app changes only mcla.exe's copy of the setting, and the GPU plugin, which has its own, keeps vsync on. Its emulated 60 Hz vblank then caps the game at 60 and paces it unevenly at any target.

## Timer resolution kept while the window is hidden (2026-10-14)

Windows 11 stops honouring `timeBeginPeriod(1)` for a process none of whose windows can be seen: minimized, fully covered, or with the display asleep. The timer then runs at 15.6 ms, every limiter sleep overshoots, and frames come out at 16 ms or more whatever the target. `MclaApp::ConfigureFrameTiming` now opts out with `SetProcessInformation(ProcessPowerThrottling, PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)` before raising the resolution and logs whether that worked. Checked with `NtQueryTimerResolution` while the display was asleep: 15.625 ms before, 1 ms after.

## Achievement pop-ups made the presenter repaint at the monitor's rate (fixed 2026-10-14)

Overlays showed 240 FPS on a 240 Hz display while the game made 30 to 60 frames. The SDK registers its achievement toast as an ImGui dialog for the whole session, and while any dialog is registered the ImGui drawer asks for another paint after each one, so the presenter showed every game frame several times. Besides wrong overlay numbers, the extra presents went to the queue the GPU command thread submits to and spoiled frame pacing. (mcla-recomp's open Linux pull request reports MAILBOX presenting about 1,000 times a second, which fits the same cause.)

`src/achievement_toast.h` wraps the SDK's toast and registers it only while an achievement is on screen, an approach ported from ForzaHorizon1-ReImagined (MIT; notice in the file). With `patches/rexglue-imgui-dialog-repaint-on-demand.patch` the toast is also drawn with the game's own frames instead of asking for repaints while it shows. Windows, D3D12, 60 cap: presents equal game frames before, during and after a toast (60 a second; 240 during a toast without the SDK patch), and 99.4 to 100% of frames within 1 ms of the target while one shows. The unlock itself is saved before the toast appears (`XGIUserWriteAchievements`, `Documents\mcla\achievements\545407F8.toml`).

## Motorcycle lean above 30 FPS (2026-10-14)

`mcBikeGyro::Update` (sub_823688B8; identified by LARecomp from the game's RTTI, its pull request #9) eases a smoothed position and velocity toward the bike's by a fixed fraction per frame: 0.40 and 0.15 when the rounded frame rate is 60 or more, doubled below that. That matches the console only at 30. At 120 the position settles about 1.3 times and the velocity 1.8 times as fast, at 45 both 1.5 times, and at a 60 cap frames that round to 59 switch to a filter two to three times as stiff. It was the one frame-rate switch in LARecomp's list with no correction here.

Two mid-asm hooks (`mcla_bike_gyro_pos_smoothing` at 0x8236895C, `mcla_bike_gyro_vel_smoothing` at 0x823689F0, where both ways through the game's test meet) replace each factor with the console's 30 FPS fraction applied by elapsed time, `1 - (1 - k30)^(dt * 30)`, as for the camera filters above. Written for this project from those facts, not copied (LARecomp has no licence). `--mcla_bike_gyro_fix=false` turns it off.

Checked: the hook sites in the generated code, the constants read at run time (2.0, 0.40, 0.15), and with a temporary probe at live 30, 60, 120 and uncapped frame rates, exactly 0.2 (position) and 0.7 (velocity) remaining per 1/30 s in every case. **Not verified by riding**: the function runs only with a motorcycle in play.

## Attract movie: other shots' colours over the picture with hot pages (fixed 2026-10-14)

Reported by the user from a test run: towards the end of the attract movie (the one that plays after idling on the title screen), a flashing pinkish outline over the picture. Window captures of the whole movie, compared with `attract720.bik` decoded by FFmpeg, located it: in runs with hot pages on (`--shared_memory_hot_page_faults=4`, the PS5's setting, tried on PC while profiling) the brightness of each frame was right but its colour came from earlier shots, so red and blue blobs and pink outlines of other shots' shapes lay over the current one (a green motorcycle turned grey with red circles beside it). Without hot pages, with every other setting of this fork on, all captures matched the movie. The flash-white frames with a pinkish smudge beside the rider at the very end, and the venetian-blind wipe a few seconds before, are in the movie itself.

Cause: a texture loaded from a hot page was never reloaded while the page stayed hot, as no write fault told the texture cache it had changed (details in `patches/README.md`, `rexglue-hot-pages-watched-ranges.patch`, which fixes it). Bink writes the movie's planes into textures every frame, so the colour planes stuck on old shots. Hot pages are off by default on PC; the PS5, which runs with them, should have had the same fault.

## Mouse pointer over the game during play (2026-10-14)

Playing with a controller, the mouse pointer stayed in the middle of the picture until it was moved off the screen. The game now hides it the way PC games do. It goes as soon as a controller is used, or once the mouse has been still for about 3 seconds, and comes back as soon as the mouse moves or is clicked. It is hidden only over the game's picture while the game has focus. With another window in front, over the title bar or borders in windowed mode, or while the SDK's console, settings or achievements overlay is open, it stays. `mcla_hide_cursor` (default on; `--mcla_hide_cursor=false` keeps the pointer) selects the SDK's auto-hidden cursor, which `patches/rexglue-cursor-auto-hide.patch` makes focus-aware and lets the pads hide (details and checks there). **Not verified by hand**: the controller path, as unattended runs have no pad to press.

## Vulkan on Windows (2026-10-14)

`--mcla_gpu_backend=vulkan` (or the launcher's *Graphics API*) runs everything of this fork on Vulkan: the frame pacing, the toast and cursor changes and every SDK patch that applies to Vulkan were checked there, and the launcher's settings help on Vulkan at least as much as on D3D12 (`clear_memory_page_state=false` alone took 46% off the frame time per draw at native). Two SDK changes came out of it: the Vulkan backend now counts draws and the other per-frame counters (`rexglue-perf-counters-batched.patch`), and its draws no longer look a setting up in the cvar registry each (`rexglue-vulkan-memexport-check-order.patch`, 6% less GPU command thread time per draw). The hot-page fix for the attract movie works the same on Vulkan, where the PS5 needs it.

Two more Vulkan changes followed (2026-10-15), measured in the city with the scripted controller: shared memory uploads go before the render pass instead of ending it (`rexglue-vulkan-hoist-shared-memory-uploads.patch`; with hot pages every draw's upload had ended the render pass, about 800 times a frame; free roam 19 to 27% faster), and command buffers are recorded and submitted on their own thread as on D3D12 (`rexglue-threaded-submission-vulkan.patch`). On this machine (RTX 5080) Vulkan then matched D3D12 at native resolution and was 7 to 11% slower at 3x, where its texture loads took about three times as long on the GPU (fixed since, see below); it stuttered less than D3D12 in the map zoom-out after the title until D3D12's textures were placed in shared heaps (next sections). Validation (`--vulkan_validation_enabled=true`) reports three kinds of message, all present with every change of this fork off: a pixel shader output with no attachment (warning), an occlusion query begun outside a render pass and ended inside one, and a swapchain destroyed while in use on fullscreen switches. Measurements and method: `launcher/dev/vulkan-2026-10-14.md` in the fork. **Not verified**: races and the other places that need a player.

## Loading-screen pacing: uncapped stutter and the fast intro (fixed 2026-10-14)

Two faults in `PaceUnlimitedSwap` (`src/frame_timing.cpp`), the hold that keeps loops which never reach the engine timer (the start-up legal screens and logos, loading screens) at the console's 30 per second:

- Uncapped with the intro at `normal`, about a quarter of all frames took 33 ms. The hold took any swap with no pass through the frame limiter since the previous swap for such a loop. But the game updates and presents on different threads, and uncapped the two drift against each other: in normal play about one swap in forty has no update before it and the next one has two (counted per swap with the dev pad's frame log: zero-update swaps came only singly, never two in a row, in the title, the map zoom-out and free roam). Holding each of those to 33 ms also delayed the presenting thread, which made the pattern about ten times as frequent. Free roam, native, D3D12, uncapped: median 4.6 ms but mean 81 FPS and 26.7% of frames over 30 ms before; median 6.8 ms, mean 145 FPS and no frame over 30 ms after. The map zoom-out went from a mean of 66 to 132 FPS (frames over 30 ms 34.8% to 0.7%). A swap is now held only when the one before it had no update either, which a real untimed loop always shows.
- `mcla_intro=fast` (and `skip`) turned the hold off for good, so every later loading screen also ran as fast as the PC could present. They now leave it off only until the engine timer first runs, which is the end of the start-up logos.

Unchanged: the start-up sequence at `normal` (334 swaps over 11.3 s at 29.4 per second, against 327 over 11.1 s before), at `fast` (879 swaps in 3.7 s, against 893 in 3.8 s), and capped play, where every swap has exactly one update (120 cap, free roam: 1,200 of 1,200 swaps). In the reachable path (start-up, title, attract movie, map zoom-out, free roam) the start-up sequence is the only untimed loop, so the fast intro's new limit was checked by reading the code, not on a later loading screen.

## Menus and HUD animations ran faster above 30 FPS (menus fixed 2026-10-14, the HUD optional)

Reported by the user at a steady 120 cap: the song title scrolling under the speedometer ran far too fast while the game itself ran at normal speed. Every UI screen is a Flash movie (menus, pop-ups, the HUD with its song ticker and "LOADING" badge), and the UI manager steps each one with the Flash player's one-frame mode, which advances exactly one movie frame per game update and ignores the elapsed time (from LARecomp's notes, used with its author's permission: mcUIManager::Update, sub_821FC588, passes byte_827DC1FC = 1, which nothing writes, to the advance, sub_825EDC08). The console updated 30 times a second, so at 120 FPS every Flash animation ran four times as fast.

`mcla_ui_movie_advance` (a mid-asm hook at 0x825EDC6C in the advance, `src/frame_timing.cpp`) now gives each of the UI manager's screens (menus, the pause menu, pop-ups, prompts, the GPS map's panels; found by name in its movie map) its own 30-per-second schedule; an update that comes before a movie is due skips that movie's advance, through the same exit the player takes when a movie is not due. A movie due within a quarter of a frame is advanced early, so at 30 FPS every update still advances once. A movie that falls behind (below 30 FPS) is not caught up, as on the console. The start-up logos are left out, so the fast intro still runs them as fast as possible. `--mcla_ui_timing_fix=false` restores one movie frame per update.

Measured with the dev pad's frame-by-frame screenshots, timedemo at each cap (`--mcla_fixed_dt` = the cap), native, D3D12:

| | 30 FPS | 120 FPS before | 120 FPS after |
|---|---|---|---|
| Song ticker under the speedometer (px/s at 720p), with `mcla_ui_timing_hud=true` | 59.9 | 239.9 | 60.0 |
| Pause menu opening (Start in the city) | 933 ms | 183 ms | 883 ms |

The 30 FPS figures are the same with the fix on and off (59.5 to 59.9 px/s). The car's path over the scripted drive from the spawn point differed between runs with the fix on and off no more than between runs with the same setting (up to 13 m apart either way; the closest pair, 0.4 m, was one of each), and the drive's frame time was unchanged. Short presses still reach the menus: the GPS map's legend toggled on each of eight 15 ms presses of X at 120 FPS with the fix on, as with it off.

What else this covers and what it does not:

- Changed with it (the same mechanism, not measured separately): pop-ups and notifications, the GPS map's panels, the title screen's prompts.
- Not affected: the minimap, which the game moves itself (it changed on every new frame with the fix on, as off), and the GPS map's zoom-out, which is a camera move.
- Not affected: the 3D icons over the world (`3DICONSMOVIE0`-`2`), which the game advances by elapsed time already.

The in-game HUD is paced only with `--mcla_ui_timing_hud=true` (default off). It is two movies: HUDMAPMOVIE and the gauge movie, which the HUD code creates itself, outside the UI manager's screens. The game sets the gauges' values every update (`GearValue`, `Nitrovalue`, `map_rotation` and so on, seen by logging the player's variable lookups) and the movies' own scripts draw them as they advance, in the same frame step (`sub_825FB0D8` on the root sprite, one call for timeline and scripts) that scrolls the song ticker. Each of the two movies is needed for both: with either one paced alone, the ticker ran at 60 px/s and the needle stepped. Splitting the timeline step from the scripts would mean changing the Flash player's frame step, so the HUD is either paced or not:

| 120 FPS | Song ticker | Tachometer needle |
|---|---|---|
| `mcla_ui_timing_hud=false` (default) | 239.9 px/s, as before | moves on every frame (69 of 116 frame pairs, the same as with no pacing at all) |
| `mcla_ui_timing_hud=true` | 60.1 px/s, as at 30 FPS | moves 30 times a second (28 of 119 frame pairs, in steps of about 2 px instead of 0.7 at 640x360) |

The menus stay paced either way: the pause menu opened in 883 ms at 120 FPS with the default settings. LARecomp left its HUD out of the same kind of pacing for the same reason. The "LOADING" badge belongs to the HUD and follows the same setting.

## D3D12 stutter in the map zoom-out and in races (fixed 2026-10-15)

On D3D12, the map zoom-out after the title had frames of 150 to 330 ms where Vulkan had 30 to 45 ms (1% low 9 against 31 FPS at 3x, 19 against 48 at native, real time), and races had frames of 60 to 140 ms. Every new texture was a committed resource, an allocation of its own that took about 0.19 ms to create and 0.08 ms to release. The zoom-out streams in 750 to 900 textures in a single frame when the city appears, and at 3x the texture cache goes over its memory limit and evicts 1,400 to 1,500 at once; races stream textures in as the car moves. Vulkan sub-allocates its textures from large blocks. `patches/rexglue-d3d12-placed-texture-heaps.patch` places D3D12's textures in shared 256 MB heaps the same way (setting `d3d12_texture_heaps`, default on). Map zoom-out, real time, 1% low: 54 FPS at native and 27 FPS at 3x (Vulkan 48 and 31), worst frame 44 and 47 ms. Race, 1% low: 50 to 96 FPS at native and 41 to 61 FPS at 3x, worst frame 102 to 13 ms and 61 to 20 ms. Median frames and 60 FPS pacing are unchanged. One frame of about 55 ms remains in the zoom-out on both APIs, about 3 s in; it shows no GPU work, so it is the game's own. Measurements and method: `launcher/dev/d3d12-zoomout-2026-10-15.md` in the fork; details in `patches/README.md`. **Not verified**: other GPUs; the heaps hold video memory in 256 MB steps, which matters most on cards with little of it.

## Vulkan texture loads slower than D3D12 at 3x (fixed 2026-10-15)

At 3x, Vulkan's texture loads took 4.4 ms of each GPU frame, against 1.6 ms on D3D12. GPU timestamps around each step of the loads put almost all of it in one place: the copy of a decoded texture into its image (`vkCmdCopyBufferToImage`) ran at about 120 GB/s on NVIDIA's Windows driver, against 400 to 870 GB/s for D3D12's copy. The textures loaded every frame are render targets the game resolves into textures, up to 66 MB each at 3x.

`patches/rexglue-vulkan-texture-load-storage-copy.patch` writes the single-level ones with a compute shader through an integer view of the image instead, bit for bit the same (setting `vulkan_texture_load_storage_copy`, default `auto`: on with NVIDIA only). In the scripted city run at 3x, frames got shorter:
- free roam: 14.9 to 12.0 ms (-19%);
- the drive: 15.1 to 12.2 ms (-19%);
- the map zoom-out: 13.1 to 11.4 ms (-13%).

1% lows rose from 59-60 to 71 FPS. At 3x Vulkan is now 10 to 14% faster than D3D12 (with D3D12's placed texture heaps); at native the two are the same.

Validation shows nothing new; screenshots and the attract movie look the same. Measurements and method: `launcher/dev/vulkan-2026-10-14.md` in the fork; details in `patches/README.md`.

**Not verified**: other GPUs and drivers, which is why it is on with NVIDIA only by default, and the bike.

## Clears drawn at 4x MSAA took a quarter of the GPU frame at 3x (fixed 2026-10-15)

The game clears its render targets the Direct3D 9 way, by drawing a rectangle over them, mostly with 4x MSAA over targets drawn with 2x (the main scene, in two tiles) or 1x (the HUD's depth). The emulator's host render targets can't change MSAA mode in place. So each such clear copied the whole range into a 4x MSAA target, drew there, and copied it all back for the next draw. At 3x that was 3.3 to 3.6 ms of a 13 to 14 ms GPU frame on D3D12, and 2.6 to 2.7 ms on Vulkan.

`patches/rexglue-clear-rectangle-draws.patch` does these draws as clears of the host render targets, like a resolve's clear (setting `clear_rectangle_draws`, default on, both APIs). It clears in the MSAA mode that the range was drawn with next after the same clear last time, so the scene's tiles are cleared in place with no copies. Timedemo at 3x, median frame:
- **D3D12:** standing in the city 14.0 to 10.7 ms (-24%), driving 13.8 to 10.3 ms (-26%), the map zoom-out 12.7 to 8.7 ms (-31%), the race 12.9 to 10.5 ms (-18%).
- **Vulkan:** 12.1 to 9.8 ms (-19%), 12.3 to 9.8 ms (-20%), 11.3 to 7.7 ms (-32%), 13.6 to 11.9 ms (-13%).
- **1% lows** rose with them, 59-61 to 74 FPS on D3D12 and 69-70 to 82-84 on Vulkan in the city.
- **Native and pacing:** native resolution and 60 FPS pacing are unchanged.

Screenshots taken with the setting switched on and off within one run look the same. The D3D12 debug layer and Vulkan validation report nothing new. Measurements and method: `launcher/dev/edram-transfers-2026-10-15.md` in the fork; details in `patches/README.md`.

**Not verified**: other GPUs.

## Resolved textures loaded in two passes at 3x (fixed 2026-10-15)

With resolution scaling, the game's render targets resolved to memory are decoded into textures again at their next use, and at 3x the screen-sized ones are reloaded every frame, some several times, because the game reuses that memory for other targets. Each load decoded into a scratch buffer and then copied that into the texture: 1.6 ms of the GPU frame on D3D12 and 1.85 ms on Vulkan, standing in the city.

`patches/rexglue-texture-load-direct.patch` decodes these textures straight into themselves through an unsigned integer view, in one pass (setting `texture_load_direct`, default on, both APIs; on Vulkan with the storage views of `vulkan_texture_load_storage_copy`, so NVIDIA only by default). Texture loads went to 1.1 ms on D3D12 and 1.4 ms on Vulkan. Timedemo at 3x, median frame: D3D12 10.6 to 10.2 ms standing in the city (-4%), 10.2 to 9.7 ms driving (-5%), 9.7 to 8.1 ms in the map zoom-out, 10.4 to 9.9 ms in the race; Vulkan 9.8 to 9.4 ms (-4%), 10.0 to 9.4 ms (-6%), 8.8 to 7.9 ms, 11.9 to 11.6 ms. Native resolution and 60 FPS pacing are unchanged.

Screenshots taken with the setting switched on and off within one run look the same; the D3D12 debug layer and Vulkan validation (with synchronization validation) report nothing new. Measurements, what drives the resolves and the reloads, and the options left: `launcher/dev/resolves-2026-10-15.md` in the fork; details in `patches/README.md`.

**Not verified**: other GPUs.

## Stutter while new shaders compile (fixed 2026-10-15)

On a first run, in new places or races, and after a driver update or a change to the translated shaders, the GPU emulation creates new pipelines, which the driver compiles. Draws whose pipeline was still being created were already left out, but at the end of every frame both APIs then waited for every pending pipeline, holding the frame for the slowest compile. Vulkan also compiled a stand-in pipeline for each one on the GPU command thread first. With the driver's cache cold:
- D3D12 frames of up to 330 to 400 ms;
- on Vulkan, up to 1.9 s, with 17 s a session in frames over 50 ms; the map zoom-out fell 1 to 2 s behind.

`patches/rexglue-async-pipelines-no-frame-wait.patch` stops the frame from waiting. A draw waits for its pipeline only within 4 ms a frame (`async_pipeline_wait_budget_ms`), and is otherwise left out until the pipeline is ready. Vulkan no longer creates the stand-in pipelines. A session started with nothing cached:
- **D3D12:** worst frame 160 to 190 ms instead of 330 to 400, half as much time in frames over 50 ms, map zoom-out 1% low 23 to 26 FPS instead of 13 to 19.
- **Vulkan:** worst frame 170 to 250 ms instead of 1.9 s, 0.4 s in frames over 50 ms instead of 17 s, race 1% low 51 to 55 FPS instead of 13 to 15.
- **What shows instead:** objects missing for a moment while their pipelines compile. In one screenshot of a cold Vulkan zoom-out, some buildings were missing.
- **The remaining slow frames:** the remaining frames over 100 ms are also there with everything cached (one at the title screen and the race's loading).
- **Warm sessions:** with the shader storage and the driver's cache warm, as in normal play after the first runs, nothing is compiled during play and nothing changes.

`patches/rexglue-vulkan-pipeline-storage-parallel-translation.patch` translates the stored shaders in parallel when Vulkan starts: 0.3 s to 0.05 s of every start.

`patches/rexglue-vulkan-pipeline-cache-file.patch` can keep the driver's pipeline cache between runs (`vulkan_pipeline_cache_file`, off by default). NVIDIA's driver has a cache of its own, so it gained nothing here; it is meant for drivers without one.

Measurements and method: `launcher/dev/shader-stutter-2026-10-15.md` in the fork; details in `patches/README.md`.

**Not verified**: other GPUs and drivers.

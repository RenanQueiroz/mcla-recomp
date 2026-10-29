# Status, testing and known issues

The short version is in the [README](../README.md#status). This page has the rest: what was tested on what, what each target does, frame times on the test machine, and the known issues in short. The full list of known issues and how each change was checked is [KNOWN-ISSUES.md](../KNOWN-ISSUES.md).

## Testing so far

Everything this fork adds was built and measured on **Windows only**, with an **NVIDIA RTX 5080** and a **Ryzen 7 9800X3D**, on both Direct3D 12 and Vulkan. It has also been run on the Ryzen's integrated AMD Radeon graphics (both APIs, 720p to 4K), where the white or grey flashes on AMD with Direct3D 12 were found and fixed in v0.1.3, but not on a discrete AMD card, on Vega-based Ryzen graphics, on Intel graphics, on Linux or on the PS5 yet. If you try it on other hardware, please report how it went in the [fork's issues](https://github.com/RenanQueiroz/mcla-recomp/issues), whether it works or not.

## Status

| Target | State in this fork |
|---|---|
| Windows, Direct3D 12 | Plays: career, races, garage and upgrades, with audio and a controller. 30 to 120 FPS or uncapped, at native resolution (720p), 2× or 3× (4K). |
| Windows, Vulkan | Runs with the same settings, through the same scripted runs as D3D12: the map zoom-out, free roam, a drive and a race. Not yet played through by hand. (Upstream lists it as reaching the title screen.) |
| Linux, Vulkan | Upstream's build plays: free roam holds 30 FPS on an Intel Core Ultra 5 238V laptop (Mesa ANV), tested by one user. The fork's SDK patches apply on top, but have not been built or run there. |
| PS5 (jailbroken) | Upstream's build plays at a steady 30 FPS on a PS5 Pro (firmware 13.42), 20 to 25 in the zoomed-out map, and was installed and played on a PS5 Slim (firmware 12.70). The fork's `make_ps5.sh` also applies the fork's SDK patches, which have not been built or run on a console. |

## Frame times on the test machine

### v0.3.0 against v0.2.1

From the v0.3.0 release notes. Test PC: Ryzen 7 9800X3D, RTX 5080, Windows 11, and the Ryzen's integrated Radeon graphics. Uncapped, median frame time, the same free-roam and driving route:

| | v0.2.1 | v0.3.0 | Change |
|---|---|---|---|
| RTX 5080, Direct3D 12, native, free roam | 4.61 ms | 4.16 ms | -9.7% |
| RTX 5080, Direct3D 12, native, driving | 4.32 ms | 4.26 ms | -1.5% |
| RTX 5080, Direct3D 12, 3x, free roam | 8.00 ms | 5.99 ms | -25% (125 to 167 FPS) |
| RTX 5080, Direct3D 12, 3x, driving | 7.82 ms | 6.21 ms | -21% |
| RTX 5080, Vulkan, native (free roam / driving) | 4.22 / 4.63 ms | 4.23 / 4.68 ms | no change (within the run-to-run variation) |
| Radeon, Direct3D 12, native, free roam | 37.7 ms | 35.6 ms | -5.7% |
| Radeon, Direct3D 12, native, driving | 36.9 ms | 34.3 ms | -7.0% |

At the default 30 FPS cap, the game's CPU time for each frame (free roam / driving):

| | v0.2.1 | v0.3.0 | Change |
|---|---|---|---|
| RTX 5080, Direct3D 12, native | 64.5 / 64.8 ms | 15.9 / 19.7 ms | -75% / -70% |
| RTX 5080, Direct3D 12, 3x | 62.5 / 63.9 ms | 15.8 / 17.7 ms | -75% / -72% |
| Radeon, Direct3D 12, native* | 87.3 / 85.3 ms | 46.4 / 34.5 ms | -47% / -60% |

On the RTX 5080 the frame rate held at 30 FPS with the same pacing. *The integrated Radeon can't quite reach 30 FPS at native resolution; it went from about 26 to 28 FPS.

### Earlier figures

The paragraph below is from the mid-October builds (before v0.2.0); the tables above are newer. Its method and per-change figures are in the notes it names.

On the test machine, with the launcher's settings: at native resolution the game's own thread sets the pace, at about 4.2 ms a frame in free roam (about 240 FPS uncapped; v0.3.0 notes: 4.16 ms on Direct3D 12) where it was 6.8 ms (about 145 FPS) before the frame-pacing thread was woken on demand. At 3× (4K) a 120 FPS cap holds in free roam and while driving in the city, with 93% of frames within 1 ms of the target on D3D12 and 97% on Vulkan (99% on both standing in the city by day since single-tile rendering). Races at 3× run at a median of 8.1 to 8.5 ms a frame on D3D12 and 9.3 to 9.6 ms on Vulkan in the race benchmark (`launcher/dev/scripts/race.pad`, uncapped, the release build, two runs each); with the scene in two tiles 9.7 to 10.1 and 10.9 to 11.1 ms, where the runs of 2026-10-15 gave 9.3 and 10.8 (the benchmark's runs spread by up to 8%). Method and per-change figures: [patches/README.md](../patches/README.md) and `launcher/dev/single-tile-edram-2026-10-16.md`.

## Known issues in short

[KNOWN-ISSUES.md](../KNOWN-ISSUES.md) covers each of the fork's changes: what it fixes, how it was checked and what is not verified. In short:

- On AMD graphics the fork has only been run on the Ryzen 7 9800X3D's integrated Radeon (RDNA 2; D3D12 and Vulkan), not on a discrete AMD card or on Vega graphics (the Ryzen 3000G to 5000G APUs: a Ryzen 7 5700G user saw the HUD, the menus and parts of the city missing on Vulkan, fork issue #14, so Automatic takes Direct3D 12 there; untested here); it hasn't been tried on Intel graphics, on Linux or on the PS5. On Vulkan, the compute texture copies and the direct texture loads are on with NVIDIA only by default (`--vulkan_texture_load_storage_copy=on` tries them elsewhere). D3D12's texture heaps take video memory in 256 MB steps, which matters most on cards with little of it.
- On a first run, in new places, or after a driver update, objects can be missing for a moment while their shaders compile, where the game used to hold the frame until they were ready. With the shader pack (added at every start, compiled by the launcher before Play when needed) that is left only for what the fork's test sessions didn't reach.
- The HUD's gauges and the song title under the speedometer move in the same steps. At *HUD updates* 60 per second (the default) the needles are smooth and the title scrolls twice its original speed; at 30, the console's pace, the title scrolls at its original speed and the needles move 30 times a second; at 120 the title scrolls four times as fast.
- Not verified by hand: the motorcycle lean (it needs a bike in play).
- On Vulkan, while the camera was turned round a moving car, the car's plate and rear lights could look blurred where the speedometer's box is; D3D12 didn't show it. Fixed in the SDK (`vulkan_odepth_float24_scale`, on by default; the cause is in [KNOWN-ISSUES.md](../KNOWN-ISSUES.md#a-soft-smear-over-a-cars-plate-and-rear-lights-next-to-the-speedometer-on-vulkan-issue-11-fixed-in-the-sdk-2026-10-28)). Seen on an RTX 5080 and a Radeon iGPU (pictures with the fix on and off in `launcher/dev/vulkan-plate-blur-2026-10-28.md`); no cost that could be measured (+0.5% and +0.9%, inside the spread between equal runs).
- The vinyls of the arcade race's opponents are restored (D3D12 and Vulkan; see [KNOWN-ISSUES.md](../KNOWN-ISSUES.md#custom-vinyls-missing-on-cars-outside-the-vinyl-editor-issues-11-and-14-fixed-2026-10-27-for-the-arcade-races-opponents)); the vinyls you make in the editor and the story's cars were not seen with it. At 2x and 3x resolution the liveries came out coarse in v0.2.0 and are smooth now (seen at 2x on D3D12 and Vulkan; not seen in a picture at 3x).
- At 2x and 3x resolution the rain was not drawn (fork issue #17); it is now, on D3D12 and Vulkan (seen at 2x and 3x in the rainy arcade race of an RTX 5080; the details and what was not checked: [KNOWN-ISSUES.md](../KNOWN-ISSUES.md#rain-not-drawn-above-native-resolution-issue-17-fixed-2026-10-28)).
- At 2x and 3x resolution the game takes a lot of video memory (v0.2.0, RTX 5080: 3.9 to 4.0 GB at 2x, 7.5 GB at 3x, against 1.7 GB at native), and a 6 GB card (the reporter's) slowed to about 5 FPS in the pause menu and at race transitions (fork issue #19). Three SDK options now cap the render targets' height, free the unused ones and answer to the OS's budget; they are on by default. Measured on an RTX 5080 only: against v0.2.0 the peak went from 4.0 to 2.7 GB at 2x and from 7.5 to 5.0 GB at 3x on D3D12 (Vulkan: 3.9 to 2.7 GB and 7.5 to 4.8 GB); with a card pretended to have 3.2 GB the pause menu ran at 155 FPS instead of 0.8, and with 2.6 GB pretended the pressure response triggered and kept free roam at 173 to 217 FPS (36 to 50 with it off). **Not confirmed on a small card** (the reporter's RTX 3060 laptop); the pretended card is D3D12 only, and a race, FSR, HDR and frame generation were not compared by picture with the cap; the details and what was not checked: [KNOWN-ISSUES.md](../KNOWN-ISSUES.md#video-memory-above-native-resolution-a-few-fps-on-a-6-gb-card-issue-19-fix-added-2026-10-28).
- HDR is new: on by default on Windows whenever Windows has HDR on for the display (signed off on one display, an ASUS ROG Swift PG32UCDP (WOLED, Gaming HDR mode, Uniform Brightness on), 2026-10-16), it outputs HDR10 only (no scRGB), it works on Windows only for now (the PS5 keeps SDR; Linux on Wayland comes later), and the garage, photo mode and cutscenes weren't reached in the tests. Screenshots taken in HDR come out as SDR, with the highlights clipped.

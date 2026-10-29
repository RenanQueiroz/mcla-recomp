# DualSense adaptive triggers on the PC (2026-10-29, v0.3.1)

`src/pc_triggers.cpp`. On by default since the owner tried it on a DualSense (`mcla_adaptive_triggers`); both R2 buzzes are off by default, the owner preferred none: the top of a gear (`mcla_trigger_rev_limiter`, tried and found unwelcome) and the nitro (`mcla_trigger_vibration`, never tried by hand: the car had no nitro). The behaviour is upstream's PS5 triggers (`ps5/game/ps5_triggers.h`, Zaid Naeem, merged the same day);
the PC sends it to the pad through SDL3. This note is the design, what was read to get the bytes right, what the chain `pctrig` checks, and what is left.

## What each state does

| State | Source | R2 (throttle) | L2 (brake) |
|---|---|---|---|
| Driving | HUD updates seen in the last 300 ms (`g_mcla_vehicle.hud_updates`), the pause menu not up, the car not frozen, the window in front | spring: resistance zones 1 to 9 rising from 1 to `mcla_trigger_throttle` (3) | zones 2 to 9 rising from 2 to `mcla_trigger_brake` (6) |
| Top of a gear | speed above 96% of the speed the game last shifted up at in this gear (left under 93%), R2 above 220 of 255, `mcla_trigger_rev_limiter` | vibration from zone 4, amplitude strength-1, 25 Hz | as driving |
| Nitro | `nitro_time` moved in the last 200 ms, R2 above 200, `mcla_trigger_vibration` | vibration from zone 3, amplitude strength (3), 30 Hz | as driving |
| Menu, loading, no HUD for 300 ms | `hud_updates` stopped | off | off |
| Pause menu | PAUSEMOVIE advanced in the last 250 ms (`MclaPauseMenuUp`, `src/frame_timing.cpp`); or `MclaVehiclePaused()` (a moving car's velocity unchanged for 20 HUD updates, upstream's) for 200 ms running (at a few hundred FPS 20 updates are a few ms between two physics steps) | off | off |
| Window not in front | `GetForegroundWindow` is not a window of this process, `mcla_trigger_release_unfocused` | off | off |
| Option off | `mcla_adaptive_triggers` false (it can be changed during play: F4, the dev pad's `set`) | off, once | off, once |
| Game ends | `MclaPcTriggersShutdown`: the window's close button (`OnWindowCloseRequested`), QUIT GAME and RESTART GAME (`CloseGame`), the dev pad's quit, and `OnGuestThreadExit` (the title ending on its own; when the window is closed another way, `ReXApp::OnClosing` ends the process right after terminating the title and this call races it) | off, then SDL gets 60 ms to write it | same |

A pad seen for the first time with the option on is sent "off" once (a game that was killed leaves its last effect in the pad). A pad that was never sent anything is not touched with the option off.
Reports to one trigger are at least 40 ms apart (SDL's HID writer thread spends 10 ms on each report, rumble included); both triggers changing together go in one report.

Not ported: the buzzes that `ps5_triggers.h` takes from the rumble level (wheelspin on R2, ABS-like pulses on L2, a kick on both after a hit). The PC has no rumble level to read: the guest's `XInputSetState` goes through the SDK's input system into `SDL_RumbleGamepad`
(`sdl_input_driver.cpp`, `SetDeviceVibration`) without passing fork code. What it would take: an SDK patch behind an option that keeps the last vibration the guest set (two atomics read by the fork's code), about 30 lines, plus the PS5 code's rise-over-average logic. Not done: it needs a pad to tune.

Haptics from the game's sound (`ps5_haptics.h`, 409 lines) are out of scope. What it would take: the game's mixed audio (the fork's audio system `src/audio_fallback.cpp` wraps the SDK's SDL audio driver; a tap on `SubmitFrame` as the PS5 code does in `Ps5AudioDriver`), the low-passed synthesis of road, gear thump, crash and nitro (upstream's code is portable C++), and an output to the DualSense's own 4-channel audio device (USB: a WASAPI or SDL audio device named "Wireless Controller" whose channels 3 and 4 are the actuators; Bluetooth has no such device and uses report 0x32). SDL3's gamepad API has no haptics-stream call for the DualSense. A project of its own.

## How the bytes get to the pad

- The SDK's SDL input driver (`sdl_input_driver.cpp`) opens every gamepad (`SDL_OpenGamepad`). SDL refcounts an open gamepad, so `pc_triggers.cpp` takes a second reference on each pad the game's driver has opened (`SDL_GetGamepadFromID` is non-null only for those) and lets go when the pad is gone or at the end. A pad qualifies when `SDL_GetGamepadType` is `SDL_GAMEPAD_TYPE_PS5` and the vendor is Sony (0x054C): a DualSense or Edge. An Xbox pad (type 3), a PS4 pad (5) and third-party PS5-type pads are skipped, and the log says so once.
- `SDL_SendGamepadEffect(gamepad, &report, 47)` with SDL's own `DS5EffectsState_t` (`src/joystick/hidapi/SDL_hidapi_ps5.c`; the same struct in `test/testcontroller.c` of the SDL tree, whose `CyclePS5TriggerEffect` sends `{0x05, 0...}` to clear a trigger): `ucEnableBits1` = 0x04 (right trigger effect) | 0x08 (left), the 11-byte blocks at byte 10 (R2) and 21 (L2), everything else zero. Without the 0x01 and 0x02 bits the pad ignores the rumble bytes, so SDL's rumble for the game is untouched. SDL builds the USB report (0x02) or the Bluetooth one (0x31, with the CRC) and queues it to its HID writer thread (`SDL_hidapi_rumble.c`: one write per 10 ms). On USB SDL's PS5 driver is in enhanced mode from the start; on Bluetooth its first application call switches it on (a 10 ms delay once).
- Effect blocks, `[mode, 10 bytes]`: off `05 00..`; resistance `21` + active-zones mask (16 bit, one bit per tenth of the travel) + strengths (3 bits per zone, strength-1, 30 bits); vibration `26` + the same + frequency in Hz at block byte 9. The slope (zone strengths interpolated from the start zone to the end zone, then flat) is Sony's "slope feedback" computed on the host. Read for this: dualsensectl (`main.c`, `trigger_bitpacking_array`, the mode constants, the enable bits), the notes behind `Nielk1`'s trigger effect generator (as remembered; not fetched), and the SDL test above. The zone modes could not be checked against a pad by the agent; the chain checks that SDL accepts the report, not what the trigger does.
- Thread: one worker thread of `pc_triggers.cpp` (20 ms ticks, 250 ms while the option is off and no pad is held). SDL's gamepad calls take SDL's joystick lock and are safe from any thread (the SDK itself calls `SDL_RumbleGamepad` from the guest's threads). The SDK's input system has no lock of its own, so the worker never calls it: the pedals' positions are read in `mcla_frame_delta` (`MclaPcTriggersFrame`, the thread that polls the pad, only while the triggers are in use) from the game's user 0 state, the merge of every pad and the dev pad. The nitro and gear come from `g_mcla_vehicle`.
- Pause menu: a measurement from the menulag run of 2026-10-29 (`C:\mcv\menulag\dev\runs\lag\game.log`, `mcla_ui_advance_log`): after the loading screen's POPUPMOVIE ends, free roam advances no UI screen at all for the 2.6 s before the Start press (the only free-roam stretch in that log); from the press on PAUSEMOVIE is advanced every 33 ms. `mcla_ui_movie_advance` notes the time PAUSEMOVIE comes up; `MclaPauseMenuUp()` is true within 250 ms of it. The hook returns before the note when `mcla_ui_timing_fix` is off.

## The chain `pctrig` (what it checks)

`C:\mcv\pctrig\chain\chain.ps1`: builds this branch with `fast-setup.ps1` into `C:\mcv\pctrig\exe` (kept), then two runs with `--mcla_adaptive_triggers=true --mcla_trigger_log=true --mcla_trigger_fake_device=true --mcla_trigger_release_unfocused=false`, D3D12, native resolution, 60 FPS, a scratch save:

- `launcher/dev/scripts/pc-triggers.pad`: boot to free roam; idle; full throttle out of the car park and straight on (gear changes: the shift speeds are learnt); full brake; throttle again (the buzz at the top of the first gear); X held (the nitro); Start with the car moving (pause), Start again; the option off and on (`set`); the window minimised with `mcla_trigger_release_unfocused` on; the script's end quits (the QUIT GAME path).
- `launcher/dev/scripts/pc-triggers-close.pad`: boot to free roam, 4 s of throttle, `close` (WM_CLOSE, the close button's path).
- `analyze_triggers.py` reads the `pc triggers:` lines between the dev pad's marks: SDL answered true to every report (and with which enable bits), nothing but "off" before the car is driven, the idle effects, upshifts seen, the buzz at the top of a gear and for the nitro (WARN, not FAIL, if the telemetry never showed them), slack within the pause segment, off and on, the window minimised, release at shutdown before the game's hard exit, the classifier's table (only type 6 from Sony), and that no report went to a pad that is not a DualSense.
- With the owner's DualSense connected the reports also go to it and the log has SDL's answer per report; the fake pad (`mcla_trigger_fake_device`) runs the same logic and only logs, so the logic is checked even with no pad. Nobody feels the triggers in the chain.

Results: not run yet when this was written. They go below by whoever reads `C:\mcv\pctrig\out\summary.txt`.

## Not tested

A pad's feel, Bluetooth, two DualSenses, the Edge, Linux, the PS5 (nothing changed there), free roam's HUD telemetry (assumed to be the race HUD's function, `sub_822112A8`; the chain shows it), the nitro button (X, taken from `arcade-race.pad`'s TOUR).

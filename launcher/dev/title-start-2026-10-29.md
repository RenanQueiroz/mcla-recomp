# Start has to be pressed many times on the title screen (2026-10-29)

Owner's report (Xbox Series controller and DualSense, both through SDL): "I have to press Start multiple times till it's finally detected." The attract video that plays after a while of idling also took many presses to stop, and it happens after the game has run for a while.

**Status: cause not found; two workarounds written, not measured.** What follows is what the first scripted runs showed (2026-10-29, `launcher/dev/title_start.py` pads, `--mcla_pad_probe`), what was read in the recompiled code, and what the second round (trace of the key path, `mcla_title_fps`, `mcla_title_pad_hold_ms`) is set up to show.

## Measured (first round: 25 runs, 100 ms presses every 1.5 s unless noted, all threads, intro fast)

| Cap | Presses until Start was accepted (one number per run) | Accepted / pressed |
|---|---|---|
| 30 | 1, 1, 1 (also a single press, and 42 older runs with 300 ms presses) | 3 / 3 |
| 60 | 3, 2 | 2 / 5 |
| 120 | 1, 3, 2 (and with the UI timing fix off 3, 4) | 8 / 22 (all 120 runs) |
| uncapped (about 240 updates a second) | 3 | 1 / 3 |

- A single press at 120 FPS was ignored for 20 s; then 4 more presses were needed. Hold 50 ms: 5 presses (n = 1); hold 200 ms: 1 (n = 1).
- Per press the chance looks the same whatever came before (about 0.35 at 60 FPS and above), not a time after which Start works. Presses at the same update number (665 to 673) were accepted in some runs and not in others.
- The game saw every press: the UI input object logged a key down for Start at each one (once per update that the press is first seen), and the scene stack reported it taken in every title run. An accepted press flips the lighting guard (`0x82836A28`, which the dev pad's `left_title` reads) within 10 to 12 updates at 120 FPS, before the key is released. So the decision is made at the key down, in the object that takes it.
- Not the movie tick: with `--mcla_ui_timing_fix=false` the movies advance on every update and 3 and 4 presses were still needed. Not the load: the guard flips within a second of an accepted press on all threads. Not the game-state word at `0x8288D000` (0 or 2 at the title, whichever the outcome).
- Attract video (`idle120-a`, `idle120-b`; shots under C:\mcv\titlestart\out\shots, not in the repo): it starts about 14400 updates after launch; presses 1 and 2 (key down taken) did not stop it, it ended after press 3, the title came back and was left at press 7 (the shots show the title without its logo, then the map). In one run a press produced no key down at all (update 15663).
- `native_calls=0` in every title run: the first probe's hook of the script native "button just pressed" (`sub_822C7FC8`) sits after its early return for "a modal scene blocks input" (`sub_82207D30`), so this does not show that the scripts never ask. The second probe counts the calls at the entry and past that check.

## Read in the code (no run; recompiled code of the image and its RTTI)

- The pad is read once per update (`sub_8260C4D0` -> `sub_82614D30`; record of pad n at `0x82905388 + 64 n`, new mask `+8`, previous `+12`, Start = 2048).
- The UI input object (`sub_82223CD8`, once per UI manager update) sends a key down when it first sees a button pressed (Start = key 17) to `sub_82209C38`, which has three ways to take it: A, a modal scene (a ring of three at the UI manager's `+0x40724`, count at `+0x40738`): `ring[(tail + 1) mod 3]->vtable[276]`; B1, the main form (`mcMainForm`, vtable `0x82097214`, at the UI logic + `0x13F80`): `vtable[32]` = `sub_826439E0`, which hands the key to its current child form (`[this + 216]` or `[[this + 68] + 436]`) -> `vtable[276]`, mostly `sub_82631E20` (the form key handler: the form's own `vtable[32]`, then a jump table for keys 47 to 58); B2, the "UserInterface" object (`[UI manager + 40]`, vtable[276] = `sub_821F84A0`, answers 0). If none takes it, the UI input object posts event 48 + input (Start 55) with `sub_8268EE10`; the pause handler (`sub_82653058`, events 54/55/76) answers that in the city, and `sub_82652CF8` (can the game pause) refuses while the statechart is in GameState_Intro or GameState_Loading.
- The title is the UI statechart state StartScreen (`ui/UserInterface.sc.xml`): on entry `UI.ActivateAndFocus(UITransitions)`, one transition, `UI.startscreenexit` -> Startup_Checks. The C++ registers the event `EVENT_START_SCREEN_EXIT` (UI manager, `sub_821F9380`) and handles it in `sub_821FAD78` (branch at `0x821FAE10`, id at the UI manager's `+10668`). Nothing in the C++ posts it by name; no Flash movie in `xarchive_cache.rpf` (all 23,931 entries scanned) contains `startscreenexit`, `START_SCREEN` or `start_pressed`. So it is posted by a game script or by data-driven code. `MoviePlayer_StopMovie` is `sub_822CC808` (a script native), so the attract video is stopped by a script.
- Held menu buttons repeat by counting updates; with `mcla_ui_repeat_fix` on (default) the first repeat is 233 ms after the key down. The owner saw the bug on v0.3.0, where the fix is off.

## What the second round shows (`--mcla_pad_probe=true`)

For 1.5 s after every Start press: the state the key path looks at (modal count, ring, main form, its child forms, with the vtable of each; the report script names the class from a table made from the image's RTTI), the way that took the key and what it answered (A, B1 with the child and the form handler, B2), the events posted, whether `EVENT_START_SCREEN_EXIT` was handled and after how many updates, the movie player's start and stop, the movie ticks and input mapper runs in the press update, and once a second the counts of UI updates, mapper runs, movie steps and ticks, key dispatches, events, and of the "just pressed" native's calls (at its entry and past the modal check). A press is ACCEPTED when `EVENT_START_SCREEN_EXIT` follows within 1.5 s.

`python -I launcher/dev/title_start.py report <run folder>... [--vtables FILE]`. The class table is `address class` per line; it is not in the repo (it comes from the game's image).

## The two workarounds (off by default; the chain measures them)

- `--mcla_title_fps=30`: the frame-rate cap on the title screen and the attract video is 30, except for 0.4 s after a Start press (so the load after it runs at the player's cap). Reason: every measured press at 30 FPS was accepted. Risk: a tap shorter than 33 ms can fall between two pad reads at 30 FPS; the title's camera moves at 30 FPS; the load time after a press at the 30 FPS cap on 4 logical processors was long in the first round (about 19 s for the guard to flip in `lim-single`; at a 120 cap the same machine flipped it in 0.1 s), hence the release after the press.
- `--mcla_title_pad_hold_ms=40`: a press of Start or A stays "just pressed" in the pad record for 40 ms (`mcla_pad_probe_update`, `0x82615048`). Reason: a reader that looks every few updates would see it. Probably does nothing if the key path reads the UI input object's own counters, which it does for the key event.

To turn either on by default: measured in the same runs as 100 % first-press acceptance at 60, 120 and uncapped (n >= 4 each), the attract video stopping on the first press, and no second step on the screen after the title.

## Not done / not built

- Linux and the PS5 build: not built or run. `src/pad_probe.cpp` and `config/pad_probe.toml` are shared with both; the hook addresses are those of the Xbox 360 image.
- The hooks' codegen and behaviour: nothing of the second round ran. CI compiles only the host source.

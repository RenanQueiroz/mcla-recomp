# Start has to be pressed many times on the title screen (2026-10-29)

Owner's report (Xbox Series controller and DualSense, both through SDL): "I have to press Start multiple times till it's finally detected." The attract video that plays after a while of idling also took many presses to stop.

**Status: not measured.** What follows is read from the recompiled code and the owner's log; the runs that would decide it are `launcher/dev/title_start.py` pads on a build with `mcla_pad_probe` (see "How to run" below).

## What is known (from the code and logs)

- The owner's log (`mcla_ui_input_log` on, 3x, cap 120): the menu's UI input object logged a Start key down for every press (25 from 09:12:18 to 09:12:30), so the press reaches `sub_82223CD8`; the title still took until 09:12:33 and the city drew at 09:12:35.
- `left_title` (the dev pad's condition, used by every scripted run so far) is not the press. `0x82836A28` is bit 0 of the guard of a function-local static in `sub_821651B8` (lighting code, called from `sub_82165588`), set the first time it runs after the title. A run that needed 9 presses (all 42 runs at cap 30 on 4 logical processors: 16.3 s, engine frame about 412) therefore counts the presses made while the game was loading after an accepted press as well as the ones that were ignored.
- The game reads the pad once per update (`sub_8260C4D0` -> `sub_82614D30`, which first copies the old mask to `+12` of the pad record and then writes the new one at `+8`; the record of pad n is at `0x82905388 + 64 n`). "Just pressed" is `(+8 xor +12) and +8`, true for one update. The scripts read it through `sub_822C7FC8` (native table entry `0x8210C890`); the menus' UI input object reads the masks itself and keeps its own counters.
- `XamInputGetKeystrokeEx` is not how a pad reaches the title: it is called only from `sub_82614C70`, a loop that drains the keyboard's key events into a 256-byte array every update (flags `0x40000002`, user 255; the byte at `0x8281BAED` that enables it is 1 in the image). A pad's Start reaches the game through `XamInputGetState` (`sub_8275CB78`, from `sub_82614D30`) only, so the SDL driver's keystroke path (one change per call) and the analog stick thresholds are not involved in Start.
- The scripted runs at a 60 FPS cap on all threads left the title after 1, 1, 1, 2, 3 and 4 presses (300 ms holds, one every 2 s). That fits a chance of about one half per press, which a game that looks at the pad every second update (about 33 ms at 60 FPS) would give; 1 in 4 at 120 FPS. Not shown.

## Two explanations, kept apart

1. **Loading** (the figures at 30 FPS on 4 logical processors): the game accepts the first Start and loads for about 13 s before the city draws. A single press and a long wait tell it (`single` pad).
2. **A press seen in one update only** (the owner's report, any machine): a script that asks "just pressed" less often than once an update sees one press in N. `TS` lines give, per Start edge, whether a script read it (`title_start.py report`).

## How to run

Build with `mcla_pad_probe` available (this branch). Pads: `python -I launcher/dev/title_start.py pad press out.pad --hold 100 --every 1500`, kinds `repro`, `press`, `single`, `idle`. Run with `--mcla_pad_probe=true`; the candidate change is `--mcla_title_edge_hold_ms=40`. `python -I launcher/dev/title_start.py report <run folders>` prints the presses, the reads and the script's read rate.

## Not done / not built

- Linux and the PS5 build: not built or run. `src/pad_probe.cpp` and `config/pad_probe.toml` are shared with both.
- The hook addresses (`0x82615048`, `0x822C8030`, `0x822240E4`) were read in the listing; none has run yet.

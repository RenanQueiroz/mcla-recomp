# Script pad latch beyond the title screen (2026-10-29)

Question (owner): "For the button press fix, we should extend it to other areas." The title screen's Start problem (`title-start-2026-10-29.md`) comes from a game script that asks the game "was this button pressed in this update" less often than the game updates. Any script that asks the same way misses presses above 30 FPS in the same way.

**Status: the title screen is fixed and the latch is on by default (`--mcla_script_pad_latch_ms`, 100; measured, see `title-start-2026-10-29.md`). The natives are found in the code and the latch covers all of them; the default scope is `all` (owner's decision 2026-10-29), `--mcla_script_pad_latch_scope=title` limits it to the title screen and the attract video. The chain ran on 2026-10-29 (see Measured): no press was told twice, and outside the title no script asked about the buttons pressed, so outside the title the latch had nothing to do in those runs.** Which scripts call the natives, for what, is not known from the code (the scripts' bytecode is data and was not read); the chain's trace lists the call sites.

## Script natives that ask for a press

The game registers 1252 script natives (77 registration functions, each call of `sub_82554798(name, function)`; list made from the lifted code). Four of them answer "pressed in this update", true for one game update only. A search of everything they call (4 levels, against every reader of the pad records at `0x82905388 + 64 * pad` and of the action records) found no other native that reads a press; there is no "released" native (`sub_822A4D48` is "released" and `sub_822A4DB0` is "changed", both called only by C++).

| Native (script name) | Function | What it asks | Hook |
|---|---|---|---|
| `IS_PAD_BUTTON_DOWN(pad, button)` | `sub_822C8AF8` | the pad record: `((cur xor last) and (1 << button)) and cur` | `0x822C8B34` (`cntlzw r6,r8`) |
| `IS_BUTTON_DOWN(player, button)` | `sub_822C8AB0` -> `sub_822C7FC8` | the same test on the pad of the player (`sub_82388598`), after a check that no modal scene blocks input (`sub_82207D30`; blocked answers 0) | `0x822C8030` (`cntlzw r11,r3`), blocked path `0x822C7FF4` |
| `UILogic_IsPressed(player, action)` | `sub_822077F0` -> `sub_822A4CE0` | the action's record of the player's controls object: a 12-byte record at `[controls + 12] + 12 * action`; pressed = bit 7 of byte 0 and of byte 2 differ and bit 7 of byte 3 equals that of byte 0 | before the call `0x8220782C`, after `0x82207830` |
| `Player_FlashingHighbeams(player)` | `sub_822A7348` -> `sub_822A6D48` | the same record test for action 9, after conditions (a car, `sub_821E80F8`, `sub_821F7C20`): flashing the high beams, which challenges a racer | before `0x822A6DC8`, after `0x822A6DCC`, refused `0x822A6DD4` |

`GET_CONTROLLER_LEFTX` (`sub_822C8B50`) reads the stick, a level: it needs no latch. `CineScript_SKIPPING_CUTSCENE` only sets a flag. `UILogic_FireButtonPressedEvent`, `UILogic_EnableCarInput`, `UILogic_DisableCarInput` and `Racer_SetScrambleInputFlags` set things and do not ask. The cutscene skip, the race challenge and the script prompts therefore go through one of the four (which one is for the trace to say).

The C++ side (car controls, camera, the menus' key path in `sub_82223CD8`) reads the same records once per update and is not affected.

## What the latch does now

A **source** is what a native asks about: a pad's button (the first two natives) or an action record of a controls object (the last two). A **call site** is the running script thread's program (`[thread + 8]`) and program counter (`[thread + 16]`, which `scrThread::Run` stores before each opcode); a call site is told once per press:

- the pad update hook (`0x82615048`) counts the presses of each pad button; for the action records nothing counts them, so the same hook looks, once per game update, at the records that scripts have asked about (the record test above) and counts a press when it turns on; a real press that a script reads first is counted at once;
- a call site that gets the native's own answer "pressed" is marked as told;
- a call site that gets "not pressed" is changed to "pressed" once when it asked about that source within 200 ms before the press, was not told about this press, and the press is at most `mcla_script_pad_latch_ms` old (100 ms is what the title chain tests); later asks in that game update answer the same;
- a refused ask (a modal scene blocks input, no car for the high beams) takes the call site out of the pollers, so a press made while it was refused is not told to it later;
- the scope: `all` (default) applies this always, `title` only while the title phase lasts (the guard at `0x82836A28`, bit 0, is 0).

Why it should not act twice: a call site that polls every update gets the real press and is marked; the latched answer needs a press the call site was not told about. A call site that starts asking after the press is not told (so a press that closed a screen does not also act on the next one).

What it cannot help: a script that asks less often than every 200 ms (the console would miss most of its presses too), a press shorter than the time between two asks of a record action (the record shows it for one update; the pad's presses are counted at any length), a script that needs the press at the exact update.

Assumptions (the chain's log shows them): `[0x828DA3BC]` is the running thread and `[thread + 16]` the opcode's address (read in `sub_82554E20`: it stores `r28` there at every opcode); the record test equals the game's (the log prints `TS record formula differs` when it does not); the controls object's pointer is stable between asks. The chain built the 8 new hooks and ran with them. **Not checked in the game:** the record test and the controls pointer, because no script called `UILogic_IsPressed` or `Player_FlashingHighbeams` in the chain's runs (no `TS record formula differs` line, but also no record read); and `IS_BUTTON_DOWN`'s hook, which was not called either.

## The trace and the audit (`--mcla_pad_probe=true`)

- `TS script load name=... hash=...`: every script program loaded (the program's `+4` word is the hash of the name, the same function as `sub_82557F78`).
- `TS site-new native=... program=... (name hash ...) pc=... src=...`: the first ask of each call site and source; `TS sites ...`: once a second the call sites that asked, reads, real answers and latched answers.
- `TS audit ...`: for every press, every call site that asked about the source within 200 ms before it, 0.6 s later: `saw` (told once), `MISSED` (not told although it asked after the press), `gone` (not told, never asked again: the press closed the screen or the script ended) or `DOUBLED` (told in two game updates: it would act twice).
- `launcher/dev/script_pad.py report <run folder>` sums them: per native, per context (the last `mark` before the press), per call site (with the script's name), and per press.
- `launcher/dev/script_pad.py pad city|race <file>`: the pads of the chain (below).

## The chain (`C:\mcv\scriptpad\chain`, not in the repo; it ran on 2026-10-29, results in `C:\mcv\scriptpad\out`)

Runs of the dev pad from the scratch save, cap 120 and 60, each once with `--mcla_script_pad_latch_ms=0` and once with `--mcla_script_pad_latch_scope=all` (the latch's 100 ms is the default; the race pad ran at 120 only); presses at jittered times (gaps of 1.0 to 1.4 s) in: the title, free roam (a, b, x, y, lb, rb, left, right, down, four each), the pause menu (Start / B), the overhead map (d-pad up), Back, a short drive on a route with presses, and a race (the arcade's first ordered race: A while it loads and the intro runs, then the car's buttons, then the pause menu).

Read the `report` for: with the latch off, which call sites miss presses (MISSED) at 120 and 60 FPS, which native and which script (the "call sites" list); with it on, MISSED should be 0 for the call sites that ask at least every 100 ms, DOUBLED must be 0, and the screens (pause menu, map, race) must open and close as in the off runs.

## Measured

Scripted runs on 2026-10-29 (`C:\mcv\scriptpad\out\sp-summary.txt`; Ryzen 7 9800X3D, RTX 5080, D3D12, 1x, fast intro, `--mcla_pad_probe=true`, jittered gaps of 1.0 to 1.4 s, presses of 70 ms): a smoke run, the city pad (title, free roam with a, b, x, y, lb, rb, left, right, down four times each, the pause menu with Start and B four times each, the map with d-pad up six times, Back and B, a drive with six buttons twice each) at caps of 120 and 60, and the race pad (menus, the race's intro and the car's buttons, the pause menu in the race) at 120, each with the latch on (scope `all`) and off.

| Run | Latch | Title: presses missed before one was accepted | Told twice (DOUBLED) |
|---|---|---|---|
| city, cap 120 (`c120`) | off | 4 | 0 |
| city, cap 120 | on, scope `all` | 0 (the first press) | 0 |
| race, cap 120 (`r120`) | off | 8 | 0 |
| race, cap 120 | on, scope `all` | 0 (the first press; told through the latch) | 0 |
| city, cap 60 (`c60`) | off | 2 | 0 |
| city, cap 60 | on, scope `all` | 0 (the first press) | 0 |

- Outside the title nothing was audited: the only script call sites the trace saw were those of the title's script (`IS_PAD_BUTTON_DOWN`, Start and A, pads 0 to 3, one program), and only until the title was left: the once-a-second `TS sites` line, printed when a call site asked in that second, stopped there for the rest of every run. In free roam, the pause menu, the map, a drive and the race's intro and pause no script call site asked about the buttons pressed (the audit pairs each press with the call sites that polled the button), and no call of `IS_BUTTON_DOWN`, `UILogic_IsPressed` or `Player_FlashingHighbeams` was traced. So nothing was being missed there with the latch off either, and with the scope `all` the latch had nothing to do there: 0 DOUBLED outside the title says no call site was told twice, not that the latch was tested on those screens.
- No `TS record formula differs` line in any run (but no record source was read either).
- The cost of the latch with the trace off is not measured: every run had `--mcla_pad_probe=true`.
- **Not reached:** the racer challenge (`Player_FlashingHighbeams`) and cutscene skips. The pads did not flash the high beams or skip a cutscene, so nothing is measured for them, with the latch on or off.

## Not done / not built

- Linux and the PS5 build: not built or run (`src/pad_probe.cpp` and `config/pad_probe.toml` are shared; the hook addresses are those of the Xbox 360 image).
- The scripts' bytecode was not read: which scripts call which native, for what, comes from the chain's trace, which saw only the title's script call a pad native.
- The wall-clock windows (200 ms, `mcla_script_pad_latch_ms`) are in real time; a hitch of more than 100 ms between the press update and the script's next ask would miss the press as the game does.

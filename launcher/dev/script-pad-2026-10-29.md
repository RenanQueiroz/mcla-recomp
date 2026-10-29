# Script pad latch beyond the title screen (2026-10-29)

Question (owner): "For the button press fix, we should extend it to other areas." The title screen's Start problem (`title-start-2026-10-29.md`) comes from a game script that asks the game "was this button pressed in this update" less often than the game updates. Any script that asks the same way misses presses above 30 FPS in the same way.

**Status: the natives are found in the code; the latch covers all of them (`--mcla_script_pad_latch_ms`, `--mcla_script_pad_latch_scope=all`, both off by default); none of it has run in the game yet.** The third round's chain for the title (`C:\mcv\title3`) has to show first that the latch works on the title screen. Which scripts call the natives, for what, is not known from the code (the scripts' bytecode is data and was not read); the chain's trace lists the call sites.

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
- the scope: `title` (default) applies this only while the title phase lasts (the guard at `0x82836A28`, bit 0, is 0), `all` always.

Why it should not act twice: a call site that polls every update gets the real press and is marked; the latched answer needs a press the call site was not told about. A call site that starts asking after the press is not told (so a press that closed a screen does not also act on the next one).

What it cannot help: a script that asks less often than every 200 ms (the console would miss most of its presses too), a press shorter than the time between two asks of a record action (the record shows it for one update; the pad's presses are counted at any length), a script that needs the press at the exact update.

Assumptions not checked in the game (each one shows in the chain's log): `[0x828DA3BC]` is the running thread and `[thread + 16]` the opcode's address (read in `sub_82554E20`: it stores `r28` there at every opcode); the record test equals the game's (the log prints `TS record formula differs` when it does not); the controls object's pointer is stable between asks; the generated code of the 8 new hooks builds (CI compiles only the host source).

## The trace and the audit (`--mcla_pad_probe=true`)

- `TS script load name=... hash=...`: every script program loaded (the program's `+4` word is the hash of the name, the same function as `sub_82557F78`).
- `TS site-new native=... program=... (name hash ...) pc=... src=...`: the first ask of each call site and source; `TS sites ...`: once a second the call sites that asked, reads, real answers and latched answers.
- `TS audit ...`: for every press, every call site that asked about the source within 200 ms before it, 0.6 s later: `saw` (told once), `MISSED` (not told although it asked after the press), `gone` (not told, never asked again: the press closed the screen or the script ended) or `DOUBLED` (told in two game updates: it would act twice).
- `launcher/dev/script_pad.py report <run folder>` sums them: per native, per context (the last `mark` before the press), per call site (with the script's name), and per press.
- `launcher/dev/script_pad.py pad city|race <file>`: the pads of the chain (below).

## The chain (`C:\mcv\scriptpad\chain`, not in the repo)

Runs of the dev pad from the scratch save, cap 120 and 60, each once with the latch off and once with `--mcla_script_pad_latch_ms=100 --mcla_script_pad_latch_scope=all`; presses at jittered times (gaps of 1.0 to 1.4 s) in: the title, free roam (a, b, x, y, lb, rb, left, right, down, four each), the pause menu (Start / B), the overhead map (d-pad up), Back, a short drive on a route with presses, and a race (the arcade's first ordered race: A while it loads and the intro runs, then the car's buttons, then the pause menu). A control run has the latch on with the scope `title`.

Read the `report` for: with the latch off, which call sites miss presses (MISSED) at 120 and 60 FPS, which native and which script (the "call sites" list); with it on, MISSED should be 0 for the call sites that ask at least every 100 ms, DOUBLED must be 0, and the control run should miss as the off runs do outside the title.

## Measured

Nothing yet.

## Not done / not built

- Linux and the PS5 build: not built or run (`src/pad_probe.cpp` and `config/pad_probe.toml` are shared; the hook addresses are those of the Xbox 360 image).
- The scripts' bytecode was not read: which scripts call which native, for what, comes from the chain's trace.
- The wall-clock windows (200 ms, `mcla_script_pad_latch_ms`) are in real time; a hitch of more than 100 ms between the press update and the script's next ask would miss the press as the game does.

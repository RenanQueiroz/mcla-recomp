# Menu input: one press, several steps (2026-10-29)

The user's report (3x, a 120 FPS cap, D3D12; a DualSense through SDL and an Xbox controller): in every menu, one press of down moves several rows, left or right changes nothing or several values; with `--mcla_ui_timing_fix=false` the menus are responsive but still misstep at 120 FPS sometimes; with that off and a 30 or 60 FPS cap they are fine. `KNOWN-ISSUES.md` has the summary; this is the detail. "Read in the code" and "Not found" come from the recompiled game (`generated/default`, from the lifted source of an extracted `default.xex`) and from the unpacked image for data (vtables, tables), not from a run; "Measured" has the runs.

## Read in the code

The pad to the menus, in the order the game does it, once per outer update of the main loop (`sub_822C1FA8`):

| Step | Where | What |
|---|---|---|
| Pad poll | `sub_8260C4D0` -> `sub_826150F0` -> `sub_82614D30` per pad | `XamInputGetState` (wrapper `sub_8275CB78`), into a bit field per pad (`0x82815388`, 64 bytes a pad; bit `0x1000` = d-pad up, `0x4000` down, `0x8000` left, `0x2000` right, `0x40` = A, `0x20` = B). Menus do not use `XamInputGetKeystrokeEx` (`sub_8275CC50`, called by `sub_82614C70` for the keyboard state array only), so the SDL driver's own keystroke repeat (400 ms, 100 ms) plays no part. |
| Actions | `sub_8221DEA0` -> `sub_8260B990` (the action mapper) | The UI object's 24 actions (records of 12 bytes at `0x8286E4B8`: `[0]` rest value, `[2]` now, `[3]` before). A press is `(now ^ rest) > 127`. Inputs 0/1/2/3 are d-pad up/down/left/right (key codes 47, 48, 51, 52), 4/5 A and B (55, 56), 16 to 19 the left stick up/down/left/right (49, 50, 53, 54); table of key codes at `0x827DB560`. |
| Key events | `sub_82223CD8` (the UI input object's update, from `sub_82224920`, from the UI manager's update `sub_821FC588`) | Per input, in a loop over the 24: a press first seen: `counter = 0, threshold = 7`, key down (`sub_82209C38`); a release: key up (`sub_82209ED8`); held: `counter++`, and **at `counter == threshold` the key goes down again, `counter = 0, threshold = [object+803]` (6, set in `sub_8221D560`)**. Counted in updates. `0x822240C0` is the press branch, `0x822241BC..0x822241D4` the held branch. |
| Handlers | `sub_82209C38` -> the widget tree's `vtable[32]` (`sub_826309E0` for containers) | An option row (`sub_826316E0`) takes the keys 51 to 54: it changes its index (`[row+208]`) at once, with a sound, in the same update, and calls the row's delegate (the list renders again). Nothing waits for a movie. |
| Movies | `sub_825EDC08` (the advance `mcla_ui_timing_fix` gates) | Frame step of the root sprite and, when asked, the movie's own input mappers (`sub_825ED330`) and a step that reads one of their actions and walks the movie's buttons (`sub_825FA580`; what for was not worked out). A movie is advanced after the input of the same update; the C++ widgets write their state into it (`sub_82722678`, `sub_827227B8`). |

Arithmetic from that (30 a second: the console): the first repeat after 7 updates and then every 6.

| Updates a second | 30 | 60 | 120 | 240 |
|---|---|---|---|---|
| Repeat after | 233 ms | 117 ms | 58 ms | 29 ms |
| Then every | 200 ms | 100 ms | 50 ms | 25 ms |

The fork's HDR number rows count presses with `HoldSteps` (`src/settings_menu.cpp`); its own timing, so the count of game updates matters only through how often the game repeats.

## Not found

A second fault, by reading. The report says presses are lost, not only doubled. A press that is "lost" on a two-value row (a checkbox, ON/OFF) is explained by two steps. On a row with three or more values, or a list, a press that changes nothing is not. Candidates not ruled out: the movies' own input mappers (`sub_825ED330`) are ticked only on a movie advance, so with the movies at 30 a second a press shorter than 33 ms can pass between two ticks (the check's presses are 60 ms and more, so it cannot see this; the GPS legend toggled on each of eight 15 ms presses at 120 FPS in the 2026-10-14 check, which says the C++ side is fine); and the game ignoring buttons during its own transitions (the pause menu opens in 883 ms with the timing fix at 120 FPS, 183 ms without it).

## The change

`mcla_ui_repeat_fix` (on by default since it was measured, below), `mcla_ui_repeat_delay_ms` 233, `mcla_ui_repeat_interval_ms` 200, `mcla_ui_input_log` (development). Hooks `mcla_ui_key_press` (`0x822240C0`) and `mcla_ui_key_repeat` (`0x822241D0`) in `src/frame_timing.cpp`, registered in `config/frame_timing.toml`. At the press the time is recorded; at the compare, when the game's count says a repeat is due (`r8 == r10`), the repeat is allowed only after the delay (after the key down) or the interval (after the last repeat), less 8.3 ms (a quarter of a 30 Hz update); otherwise the count byte is put back by one (so the next update asks again) and `r8` is made different from the threshold (so the game does not repeat). Below 30 FPS the game's own count is the slower and decides. Wall-clock time (`steady_clock`), so a timedemo (`--mcla_fixed_dt`) is not the right way to look at it. The movies and `mcla_ui_timing_fix` are untouched.

## The check

`launcher/dev/menu_input.py` writes the dev pad and reads its log. Dev pad additions for it: `uisel` (the list the game rendered last: `select`, `scroll`, `count`, and the selected row's value slot), the conditions `ui_select`, `ui_count`, `ui_rowval`, `ui_scroll` for `wait_until` and `press_until`. The game logs, with `--mcla_ui_input_log=true`, `ui key: down|repeat input=<0..23> key=<code> t=<ms>` for every menu key event.

Each run opens the pause menu, goes to the Settings tab, and for each cell (a frame-rate cap, `mcla_ui_timing_fix`, `mcla_ui_repeat_fix`, all changed with `set` while the game runs; the pad moves about at a 60 FPS cap with 40 ms presses, which stay under the game's 7 updates, and tests at the cell's cap):

| Test | Presses | Pass |
|---|---|---|
| T0 | pause list: 4 taps of down, 4 of up (60 to 150 ms, 450 ms apart) | one key event per tap |
| T1 | PERFORMANCE (19 rows): 8 taps down, 8 up | one key event per tap |
| T2 | PERFORMANCE: down held for 1 s | 5 key events: at 0, 233, 433, 633, 833 ms |
| T3 | CITY DETAIL DISTANCE (3 values): 6 taps right, 3 left; the option read after each | one key event per tap, the value one step further each time |
| T4 | GPU WAIT YIELD (a checkbox): 4 presses of A; the option read after each | one key event per press, the value flipping each time |
| T5 | DISPLAY, PEAK BRIGHTNESS (a number row with its own hold): 4 taps right, then right held 3 s and left held 3 s; `hdr_peak_nits` read after each (A and B only) | information: the steps a hold makes |

Variants: **A** the build as shipped (`mcla_ui_timing_fix` on, the repeat fix off), **B** the repeat fix on, **C** the timing fix off (the user's workaround), **D** the timing fix off and the repeat fix on. A and B at 30, 60, 120 FPS and uncapped; C and D at 120 and uncapped. Three runs: native resolution (all of the above), 3x at a 120 cap (A to D), and native with frame generation on (FSR 3.1; the cap 60 shows 120; A and B).

The report (`menu_input.py report <run folder>`) gives, per cell, the key events of every tap, the rows moved (`uisel` before and after), the option values, the hold's event times, the update rate during the hold, and PASS or FAIL per cell. What it would show if the reading above is right: A at 120 FPS and uncapped fails (2 or 3 events for a tap, about 19 for the hold), A at 30 FPS passes, C at 60 FPS passes and at 120 FPS fails, B passes everywhere (hold 5 events). What would not fit: B failing with exactly one extra event per tap would mean the delay is too short for the hardware's frame times; a tap with no event at all would be the lost press that is not explained here, and the log of that cell then has to be read (`ui key:` lines against the `MI tap` markers, with `dev pad: press` times).

## Measured (2026-10-29)

Three runs of the pad on the build of `fix/menu-input` (RTX, Direct3D 12, a copy of the save, the pad's cells as above): native resolution (12 cells), 3x at a 120 cap (4 cells), native with frame generation (2 cells). Key events per tap (should be 1), and in a 1 s hold (should be 5). Variants: A as shipped, B the repeat fix on, C the timing fix off, D both.

| Cell | A (shipped) | B (repeat fix) | C (timing fix off) | D |
|---|---|---|---|---|
| 30 FPS cap | 1 per tap, hold 5: PASS | 1, hold 5: PASS | | |
| 60 FPS cap | 1 to 2, hold 10 (117 ms, then 100): FAIL | 1, hold 5 (233 ms, then 200): PASS | | |
| 120 FPS cap | 1 to 3, hold 20 (58 ms, then 50): FAIL | 1, hold 5 (225 ms, then 200): PASS | 1 to 3, hold 20: FAIL | 1, hold 5: PASS |
| uncapped (about 178/s) | 2 to 5, hold 30 (40 ms, then 35): FAIL | 1, hold 5 (225 ms, then 200): PASS | 2 to 5, hold 30: FAIL | 1, hold 5: PASS |
| 3x, cap 120 (115/s) | 1 to 3, hold 19 (60 ms, then 52): FAIL | 1, hold 5 (232 ms, then 200): PASS | hold 19: FAIL | hold 5: PASS |
| frame generation, cap 60 | 1 to 2, hold 10: FAIL | 1, hold 5: PASS | | |

- The prediction above held: A fails above 30 FPS, C fails at 120 and uncapped as A does (the timing fix does not change the key events), B and D pass everywhere. The first repeat is 225 ms (not 233) at the caps above 60 because the change allows it 8.3 ms early.
- No tap made no key event. The "lost" presses of the report are explained by doubled steps (two steps on a two-value row cancel). The two things listed under "Not found" were not reached by this pad (its presses are 60 ms and longer).
- A on a checkbox made one event per press in every cell. HDR PEAK BRIGHTNESS (T5): a tap is one event with the change on, 1 to 5 before; a 3 s hold ran 450 to 4,000 and back to 300 in both.
- The report's "rows moved +0" for the upward taps is the pad coming back to where it began.

On the merged build (`integrate/menus`, both menu fixes, defaults and no flags; chain `menus`, 2026-10-29, set `merged` of `menu_input.py`): the options read back as defaults (timing fix and repeat fix on), and at a 120 FPS cap and uncapped every tap made exactly one key event and a 1 s hold 5 (PASS). The control, the repeat fix turned off with `set` at 120 FPS, failed as it should. The chain `menulag` later ran the same tap and hold check in 8 more cells (caps of 120, uncapped and 60, with `mcla_ui_advance_on_input` off and on): all passed.

## Not done

- Linux and the PS5 build: `src/frame_timing.cpp` and `config/frame_timing.toml` are shared with both; not built or run here.
- A real controller: the dev pad's ScriptPad feeds the same pad state (`XamInputGetState`) the SDL and XInput drivers do, so the path above is the same, but a stick that wobbles around its threshold (the left-stick inputs 16 to 19 use one threshold, no hysteresis) can send press and release pairs; not looked at.
- A real controller by the fork's owner before a release (the pad does not show what a stick near its threshold does).

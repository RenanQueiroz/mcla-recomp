# Race start: does the countdown keep up with the real start? (2026-10-29)

**Status (2026-10-29): measured; the cause is in `race-countdown-2026-10-29.md`.** The chain ran once (the paragraph under "Measured"): the countdown ring runs at the HUD gauge movie's steps per game update, so it lags with the HUD at 60 steps a second and more with Smooth UI. This note says what was reported, what was read, what was added to measure it and how to read the result (the chain is `C:\mcv\racestart\chain\chain.ps1`). It also repeats the race finish measurement of `race-finish-2026-10-29.md` (same runs), so that chain need not be queued as well.

## The report

Testing v0.3.1 (integrate/v0.3.1 plus the PC adaptive triggers; D3D12, a DualSense, 3x resolution scale, cap 120; `mcla_ui_timing_fix`, `mcla_ui_repeat_fix` and `mcla_ui_advance_on_input` on):

- Without Smooth UI (the HUD steps 60 times a second, the screens 30): the race starts while the "2" is on screen, on every race start.
- With Smooth UI (`feat/smooth-ui`, every movie steps 30 times a second): the race starts while the "3" is on screen, and the numbers "take forever to change". Smooth, but slow.

So the countdown lags the start by about one to two seconds, a third to half of "3, 2, 1, GO", and the lag grows when the movie steps less often. Not known: whether it happens at 30 FPS, at 1x, on the console. LARecomp's commit `7b9726e` (their pacing of the UI movies, which exempts names with RACE in them) may be the same family as their "race finish in slow motion" report.

## What the code says (read, not run)

- The UI manager steps every movie with the Flash player's one-frame mode: `sub_825EDC08` with its flag set does exactly one movie frame per call and ignores the elapsed time. With the flag clear it adds the elapsed time to an accumulator at `[movie + 24]` and steps while the accumulator holds a whole frame (the frame length is `1 / swf_fps`, the halfword at `[definition + 48]`). Our hook (`mcla_ui_movie_advance`, 0x825EDC6C) sits between the two.
- The engine timer gives the game the real elapsed time (a frame is clamped at 0.125 s), so the game's clock runs at wall speed and the race starts by that clock (the mode timer, `0x827D7510`, is about 9.3 s at the start in earlier scripted races). A movie that steps `N` times per game second is `N / 30` of the way along the console's timeline if the console gave it 30.
- The pacing steps a paced movie 30 times a second of wall time (the screens, and everything with Smooth UI on) or 60 (the HUD, Smooth UI off), no more, and a movie that falls behind (under 30 updates a second) is not caught up. If the countdown is a movie of the UI manager, a lag that grows when the rate falls is what a countdown that needs more than 60 steps per game second would show. Why it would need more is **not known**: the possibilities are (1) its file declares a frame rate above 30 and the console stepped the HUD more than once per update; (2) the update rate is under 30 at 3x at the start of a race (the pipelines and textures of a new race), so the movies, stepped once per update, fall behind the clock; (3) the game's clock runs faster than the wall clock during the start (a time scale; the finish measurement reads the same field); (4) the countdown is not stepped by the UI manager at all (a clip inside a movie that stays, or the game's own drawing). The measurement tells these apart (below).

## What was added

`src/frame_timing.cpp`, `config/frame_timing.toml`, `src/dev_pad.cpp` (nothing changes unless an option is set):

- `--mcla_ui_pace_log_ms=N` (development, default 1000): the window of a `--mcla_ui_pace_log` line. The pad sets 100 for the start with `set`. Each line now ends with `us=` (the steady clock in microseconds); `steps` counts movie frames stepped.
- `--mcla_ui_movie_rates=NAME=RATE,...` (development, empty by default) now takes, besides a number, `0` and `game`: `swf` (the movie's own frame rate) and `g:RATE` / `g:swf` (RATE steps per second of the game's clock, the engine timer's total time at `0x827D7514`, instead of the wall clock). A named movie keeps a schedule that steps as many frames as are due, up to 8 in one update (a stall's backlog beyond that is dropped), where the old rule and the usual pacing step at most one. For several frames the hook turns the call into the elapsed-time call: it sets `r25 = 0` and `cr6.eq` (the branch after the hook then takes the elapsed-time path) and writes `(n + 0.5) * frame_length - elapsed` to `[movie + 24]`, so the loop steps exactly `n` times. The hook now gets `f30` (the elapsed time), `f31` (the frame length) and `cr6` as well. **This has never run**; it was read from the generated code of the advance. Smooth UI is told of one step per call (the glide covers the whole change; with a step period shorter than the frame time it does nothing, as before).
- dev pad: `burst <prefix> <n> [scale] [x0 y0 x1 y1] every=<ms>` takes a picture about every `<ms>` (up to 200; each is logged with its swap numbers and `us=`), and the `watch` line carries `us=`.
- `launcher/dev/race_start.py` (`report`, `compare`, `sheet`, `candidates`) and the start part of `race_finish.py pad`.

## The measurement

Pad (`race_finish.py pad`, the start part): `boot-to-race.pad` (Arcade, Ordered race, Sunset and Vine, 7 opponents, easiest, rain, midnight), then `set mcla_ui_pace_log_ms 100`, `watch 3` (every car's position every 3 ticks of the mode timer, 50 ms, the player first), `follow sunset-and-vine.route` (the follower holds the throttle from the intro on, as a player does), and from 1.5 s of the mode timer a picture of the whole frame every 120 ms for 14 s (scaled 0.3, 0.1 at 3x: the same size). A full run goes on with the finish part as before (the route to its end, 70 s of waiting with a picture every 5 s); a start-only run ends 6 s after the start. `mark race` comes after the pictures (about 15 s of the mode timer), so the finish reading counts from a point 5 s later than in `race-finish`.

What is read (`race_start.py report`; times are on the steady clock):

- **START**: the moment the player's car starts to move (it is 0.15 m from where it stood, keeps going to 2 m; the release time is a fit of the first metres, distance = a (t - t0)^2 / 2, so the cars' 50 ms sampling does not limit it) and the AI cars' median; the mode timer at that moment is the game's own reading. This is "the car can move": the follower holds the throttle through the countdown, so the car is held by the game until the start.
- **Movies**: for each movie stepped from 12 s before to 5 s after START: when it first and last appears relative to START, its steps, steps per second of wall time and of the game's clock while on screen over the 4 s before START, the share of asks that stepped it, and its own frame rate (`swf`). A movie that comes and goes around START is listed as a countdown candidate with its time on screen. A countdown that is a clip inside a movie that stays (HUDMAPMOVIE, the HUD gauge) shows only in the pictures and in that movie's steps per game second.
- **Clocks**: the engine timer's seconds per wall second and the updates a second over the 4 s before START.
- **Pictures**: `sheet` makes a contact sheet (a picture about every 0.25 s from 6 s before START to 3 s after; a bar on top, red before and green after START) and prints the picture times. Not analysed by the script: where the countdown is on the screen is not known (the pictures are the whole frame).

`compare` sets a run against `c30`: **LAG** is a movie that comes and goes around START and is gone more than 0.4 s later after START than in `c30` (the owner's lag is 1 to 2 s); **SLOWER** is a movie that steps 15% fewer times per game second than in `c30`; `fps-under-30` and `game-clock-differs` are the other two causes above.

Runs (RTX 5080, D3D12, HDR off, fast intro, a fresh copy of the save and of the same seeded shader storage; all with `--mcla_ui_pace_log=true`), in priority order:

| Tag | Kind | Cap, scale | Option | What it is |
|---|---|---|---|---|
| `c30` | full | 30, 1x | defaults | the reference: the console's rate |
| `c120` | full | 120, 1x | `--mcla_ui_interpolate=false` | integrate/v0.3.1's defaults: the HUD at 60, the screens at 30 |
| `c120-sm` | full | 120, 1x | Smooth UI on | every movie 30 a second |
| `c120-off` | full | 120, 1x | `--mcla_ui_timing_fix=false` | no pacing: one step per update |
| `x3` | start | 120, 3x | as `c120` | the owner's setting |
| `fix` | start | 120, 1x | as `c120` and `--mcla_ui_movie_rates=...` | the rule the earlier runs point to |
| `fix-sm` | start | 120, 1x | as `c120-sm` and the rule | the rule with Smooth UI |
| `fix-x3` | start | 120, 3x | as `x3` and the rule | the rule at 3x |
| `c60` | full | 60, 1x | defaults | if time is left |

The rule of the `fix` runs is chosen inside the chain, after `c120` and `c120-sm`: every movie that `race_start.py candidates` flags (gone later than in `c30`, or fewer steps per game second) gets `NAME=g:R` with `R` the steps per game second that `c30` gave it, stepped by the game's clock, catching up. If nothing is flagged the `fix` runs are skipped and the summary says so (then the cause is one of the others in the list above and the pictures and clocks are what there is). **Not run:** Vulkan, a slower GPU, uncapped, Linux, the PS5 build (the code is shared; not built or run there).

## Reading the result

`C:\mcv\racestart\out\rs-summary.txt` (a block per run, then its comparison with `c30`; `rs-<tag>-report.txt` per run; the contact sheets `rs-<tag>\countdown-sheet.png` and `rs-sheets.txt` after `post.py`).

| Result | Meaning | Next step |
|---|---|---|
| no LAG and no SLOWER in `c120` and `c120-sm`, START and the pictures agree with `c30` | not reproduced at 1x | look at `x3` (the owner's setting) |
| LAG in `c120` and `c120-sm`, not in `c30`, the movie's steps per game second below `c30`'s | the countdown needs more steps per game second than the pacing gives | the `fix` runs; if they show no LAG, make `NAME=g:R` the rule for that movie by default (found by name; the catch-up is in the code) |
| LAG with `fps-under-30` (`x3`) | the movies, once per update, fall behind a clock that does not wait | the catch-up of the `fix-x3` run; also a frame-rate problem at the start of a race |
| `game-clock-differs` | the engine's clock does not run at wall speed during the start | not the pacing: look at the `scale` and `clock` fields of the pace lines; the same in `c30` is the game's own |
| no movie comes and goes around START | the countdown is a clip inside a movie that stays, or not a UI-manager movie | the pictures (the sheet) and that movie's steps per game second |
| LAG in `c120-off` | not the pacing (one step per update there) | the clock and the update rate |
| START not found | the car did not move in the pad's wait | the pictures and the `watch` lines in `game.log` |

## Chain

One exclusive hold, `--max-min 40`: `C:\mcv\racestart\chain\chain.ps1` (protected files before and after, the build of `fix/race-start-countdown`, the runs of `rs_batch.py`); then `post.py` without the game. The queue commands are in `C:\mcv\racestart\NOTES.md`.

## Measured

One chain (2026-10-29, RTX 5080, Direct3D 12): `c30`, `c120`, `c120-sm`, `c120-off` as full runs, `c60` full and `x3` start only; the `fix` runs of the plan were skipped because no movie was flagged, which is right: the countdown is not a movie that comes and goes but a clip in the HUD gauge movie, which stays. START (the player's car moves) was found in every run, the game clock ran 1.002 to 1.003 s per wall second and the updates a second were steady (30, 60 or 120) in the 4 s before START. What the pictures say, from `race_start.py ring_summary` (how long each number of the ring stays; 1.0 s on the console): `c30` 1.0 s with "GO" at START; `c120` (HUD 60) 1.8 to 2.0 s; `c120-sm` (30 steps a second) 3.6 to 4.0 s; `c120-off` (a step every update) 1.0 s; `x3` (HUD 60) 1.8 to 2.0 s; `c60` (Smooth UI) 1.8 to 2.0 s. No movie of the UI manager came and went around START; the HUD gauge movie and the map were stepped once per update at cap 30, 60 times a second at cap 120 (HUD 60) and 30 times a second with Smooth UI. So the countdown follows steps per update, not steps per second: `race-countdown-2026-10-29.md` has the table, the code and the fix. The finish part of the full runs flagged no slow UI-manager movie (`race-finish-2026-10-29.md`).

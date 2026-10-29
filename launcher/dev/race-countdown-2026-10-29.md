# Race countdown: the ring runs slow when the HUD movie steps less than once per update (2026-10-29)

**Status:** the cause is measured (the race-start chain of `race-start-2026-10-29.md`, six runs) and read from the recompiled code. The fix (`mcla_ui_countdown_fix`, on by default) is measured too (chain `cdfix`, 2026-10-29, table under "Measured"): with it on "GO" was on screen at the race start in all five runs, with it off it was not. What the fix does to the clock feed is read from the logs (`cdfed / cdwrit`).

## What was reported

At a 120 FPS cap and 3x (Direct3D 12) the race started while the "2" of the countdown was on screen (Smooth UI off: HUD stepped 60 times a second), or the "3" (Smooth UI on: every movie 30 times a second), and the numbers "took forever to change". v0.3.0 is affected too (its HUD has been paced at 60 a second since 2026-10-16).

## What was measured

The race-start chain (RTX 5080, Direct3D 12, a scripted Arcade race, a picture of the whole frame every 120 ms from before the countdown on). START is the moment the player's car first moves (the throttle is held through the countdown, so the game lets it go at the end of "GO"). The ring in the middle of the screen is red for 3, orange for 2, yellow for 1 and green for GO; `race_start.py` classifies the colour of the ring in each picture (`ring_summary`). Times are from START; "a number lasts" is the time between the first picture of one colour and of the next.

| Run | Cap | HUD steps a second | Steps per update | A number lasts | GO first seen |
|---|---|---|---|---|---|
| `c30` (the console's rate) | 30 | 30 | 1 | 1.0 s | -0.05 s |
| `c120-off` (`--mcla_ui_timing_fix=false`) | 120 | 120 | 1 | 1.0 s | -0.08 s |
| `c120` (`--mcla_ui_interpolate=false`, HUD at 60) | 120 | 60 | 0.5 | 1.8 to 2.0 s | +3.00 s |
| `x3` (3x, as `c120`) | 120 | 60 | 0.5 | 1.8 to 2.0 s | +2.80 s |
| `c60` (Smooth UI) | 60 | 30 | 0.5 | 1.8 to 2.0 s | +2.97 s |
| `c120-sm` (Smooth UI) | 120 | 30 | 0.25 | 3.6 to 4.0 s | not within +7 s ("1" from +4.94 s) |

- The ring appears about 3 s before START in every run (the game starts it by its clock) and then takes 1, 2 or 4 times as long to count down: **the countdown runs at the movie's steps per game update, not at its steps per second.** 30 steps a second is in time at 30 updates a second (`c30`) and 120 steps a second is in time at 120 updates a second (`c120-off`), but 30 steps a second at 120 updates is a quarter speed.
- The game clock ran 1.003 s per wall second in all runs and the updates a second were steady (30, 60 or 120), so neither the clock nor the frame rate is the cause. The race itself starts by the game's clock, so the cars go while the ring is still on a number.
- The gauge movie (`HUDGAUGE`) and the map (`HUDMAPMOVIE`) were stepped 1 : 1 with the updates in `c30` and `c120-off`; no UI-manager movie came and went around START, so the ring is not a screen but part of the HUD.

## The cause (read from the recompiled code; the movie's own script was not read)

- `mcHudManager` (global at 0x8286E6E0, vtable 0x82029490) holds the HUD movie of each player (`hud/hud.swf`, 1 KB object created by `sub_82240E68`) in its array 1; its update `sub_8223D648` steps them in one-frame mode. In the pace log this movie is `HUDGAUGE` (it is not in the UI manager's name map).
- The HUD's own update `sub_82212298` runs every update with the update's elapsed time (the engine timer's real step in this build) in `f1`. At `0x822124E0` it calls `sub_825EE188(context, "countdownUpdate", dt)`, which stores a float in the ActionScript variable `countdownUpdate` of that movie (`[mcHudManager.movie(0) + 56]` is its swfCONTEXT, the `r31` of the movie hook). The value is `dt`, or `0`, or `[timer + 100]` in two special cases. Of the call sites of that float setter and its wrappers (24 direct, 4 tail calls, 7 through the wrapper `sub_827205B0`) this is the only one that stores the update's elapsed time; the others store values (rank, meters, a rotation, a selection, the clock's remaining time, fade speeds).
- **Inferred, not seen:** the movie's script adds `countdownUpdate` to the countdown clock on every movie step. This is what the table says: with one step per update the clock gains `dt` per step and keeps the game's time at any rate; with a step every second update it gains half, with every fourth a quarter. The variable holds only the last update's `dt` when the step reads it, so the time of the updates between two steps is overwritten and lost.
- So the console's 30 steps a second were 30 updates a second, each step adding 1/30 s. At a 120 FPS cap with Smooth UI the movie still steps 30 times a second, but each step adds 1/120 s: 30 x 1/120 = 0.25 s per second. That is why 30 paced steps a second lag while 30 updates a second at a 30 FPS cap do not: not a per-update counter in the clip, but a variable that is set every update and read every step.
- LARecomp (https://github.com/mzzvxm/LARecomp, commit `7b9726e`) found that the 30 Hz pacing breaks the HUD and race movies (they report the race finish in slow motion) and exempts movies with HUD, RACE, POST, RESULT, PODIUM, CINE or END in their names from it; their finding led to this check. For the HUD movie that gives one step per update, which the `c120-off` run above shows keeps the countdown in time; it also steps the song ticker and the needles' scripts at the update rate (the ticker at 4 times the console's speed at 120 FPS), which is why this fork keeps the HUD paced.

## The fix

`mcla_ui_countdown_fix` (default on; `--mcla_ui_countdown_fix=false` restores the game's value). Two hooks in `src/frame_timing.cpp` (`config/frame_timing.toml`):

- `mcla_ui_countdown_dt` at `0x822124DC` (right after `fmr f1,f31`, before the call that sets the variable): `r3` is the movie's context, `f1` the value about to be stored. The hook keeps `total`, the sum of every value the game wrote since the movie appeared, and `consumed`, the part of it a movie step has taken, and stores `total - consumed` instead: the game time since the movie's last step. It is at least this update's own value and at most 0.125 s (the engine's own limit for one frame), so a HUD that was hidden for a while gives its first step at most one frame's worth. It feeds only a movie that has been stepped in the last 100 writes (else the game's value stays), and values that are not a time (negative, 1 s or more, not a number) are left alone.
- The movie advance hook (`mcla_ui_movie_advance`, already there) marks `consumed = total` whenever it lets a call through for that context (a step), whatever the reason it is let through.

What it gives: with Smooth UI at a 120 FPS cap the variable the script reads is 1/30 s at each of its 30 steps a second, which is the console's value and rate; with the HUD at 60 steps a second it is 1/60 s; at a 30 FPS cap, or with `--mcla_ui_timing_fix=false`, every update steps the movie and the value is the game's own (nothing changes). **What it does not change:** how often any movie steps (so the song ticker and the needles keep their pacing), and the glide of Smooth UI (it is told the same steps). The countdown ring's own animation (the pulse of each number, the fade of "GO") is frame-driven and so runs at the movie's step rate as before: at 30 steps a second it is the console's speed (the `c30` pictures show it), while `c120-off` at 120 steps a second shows "GO" for one picture instead of four.

Logging for the check: each `--mcla_ui_pace_log` line ends with `cdn=` (times the game wrote the variable in the window), `cdwrit=` (their sum, seconds), `cdsteps=` (steps of the movie) and `cdfed=` (the sum of what those steps read); with the fix `cdfed / cdwrit` is about 1, without it `cdsteps / cdn`. A line `ui countdown: the HUD movie is now ctx=...` says which movie the hook sees; it must be a `HUDGAUGE` context of the pace lines (the hook reads the value `r3` of `sub_82212298` and the step hook `r31` of `sub_825EDC08`; they should be the same object, the vtable of `swfCONTEXT` at 0x82085300 has the advance in slot 1; that they are the same object in the running game is shown by the runs (`cdsteps` was not 0 and `cdfed / cdwrit` was 1.00 in all five); if they were not, no step would mark `consumed`, the liveness guard above would switch the feed off after 100 writes and the report would show `cdsteps=0`).

## The measurement

`C:\mcv\cdfix\chain\chain.ps1` (one exclusive hold, 40 minutes at most, about 30 expected; the queue command is in `C:\mcv\cdfix\NOTES.md`): the branch `fix/race-countdown-hud` is built from GitHub with `fast-setup.ps1`, then `rs_batch.py` runs the pad of `race_finish.py` (boot to a race, hold the throttle, a picture every 120 ms through the countdown, a fine UI pace log, `--mcla_ui_interp_stats=true`), each with the fix on: `c30` (the reference), `c120-sm` (Smooth UI, the shipping default), `c120` (`--mcla_ui_interpolate=false`) and `c60` as full runs with the finish (the route to its end, 70 s after it, a picture every 5 s), `x3` (3x, Smooth UI) as a start-only run, and the control `c120-sm-off` (Smooth UI, `--mcla_ui_countdown_fix=false`, start only). After the hold `post.py` (no game) makes the contact sheets.

**PASS** (printed by `race_start.py verdict`: `COUNTDOWN PASS` / `COUNTDOWN OUT OF STEP`): the number on screen at START is "GO" as in `c30` (a picture within 125 ms of START shows green) and GO first appears within 0.25 s of START; the ring's numbers last about 1.0 s each (the report prints them); `cdfed / cdwrit` is about 1 and `cdsteps` is not 0 (the hook and the advance agree); the HUD movies still step 30 (Smooth UI) or 60 (`c120`) times a second (the movie lines of the report); Smooth UI's own check (`Smooth UI check` line: draws with a glide to show against the glides applied) is as before; the race finish report matches `c30`'s (`race_finish.py verdict`). **The control** must still lag (about 4 s a number), which shows the pad catches the bug.

## Not done, not known

- The movie's script was not read (the SWF files are in the game's archive); that it adds `countdownUpdate` per step is inferred from the table.
- The race finish: LARecomp's finish slow motion was not seen in the earlier chain (no slow UI-manager movie; the pictures of the finish every 5 s cannot show a HUD element that lasts a second) nor in the four full runs of this one (below). The code has no other HUD variable that carries a time, so the same fault cannot be in the gauge movie by this route.
- Vulkan, other GPUs, frame generation, a real controller, split screen (the hook follows the first player's HUD movie only: `mcHudManager`'s movie 0).
- Linux and the PS5 build: not built or run; `src/frame_timing.cpp` and `config/frame_timing.toml` are shared with both and the hook addresses are the game's.

## Measured

Chain `cdfix` (2026-10-29; RTX 5080, Direct3D 12, a scripted Arcade race, one run per row, `C:\mcv\cdfix\out\rs-table.txt`), the fix on except the control:

| Run | Cap | Number on screen at START | GO first seen (s from START) | Seconds a number (3, 2, 1) | Clock feed: fed / written (steps per update) | Verdict |
|---|---|---|---|---|---|---|
| `c30` | 30 | GO | -0.07 | 0.96, 0.96, 0.96 | 1.00 (1.00) | reference |
| `c120-sm` (Smooth UI) | 120 | GO | -0.11 | 0.96, 0.96, 0.96 | 1.00 (0.25) | PASS |
| `c120` (`--mcla_ui_interpolate=false`, HUD at 60) | 120 | GO | +0.00 | 0.96, 0.96, 1.08 | 1.00 (0.50) | PASS |
| `x3` (3x, Smooth UI) | 120 | GO | -0.07 | 0.84, 1.08, 0.96 | 1.00 (0.25) | PASS |
| `c120-sm-off` (control: the fix off) | 120 | **3** | never | 3.60, 3.96 | 0.25 (0.25) | OUT OF STEP |
| `c60` (Smooth UI) | 60 | GO | -0.14 | 0.96, 0.96, 0.96 | 1.00 (0.50) | PASS |

- With the fix, the movie's steps read 1.00 of the game's time at every rate, and the number on screen at START is GO as at 30 FPS. The control still lags (the pad catches the bug).
- **The race finish** (the four full runs, `c30`, `c120-sm`, `c120`, `c60`): the game's clock ran at 1.003 s per second in the race, 1.002 for 20 s after the finish and 0.952 to 0.957 from 20 to 40 s after it in all four, the 30 FPS reference included (so the game's own slow-down, not the pacing), then 1.003. The finish's movies came as in the reference (the pause movie, pop-ups and prompts stepped as often or more often, on screen 0.74 to 1.24 times as long); the 3D icon movies are never stepped and two runs ended before they left. No slow motion by these numbers.
- **Not run:** Vulkan, frame generation, split screen, other GPUs, a real controller, Linux, the PS5.

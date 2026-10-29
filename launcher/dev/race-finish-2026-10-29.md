# Race finish: does the UI pacing slow the finish sequence? (2026-10-29)

**Status: not measured yet.** This note says what was read, what was added to measure it, and how to read the result. The measurement is one chain (`C:\mcv\racefinish\chain\chain.ps1`, see the end); nothing here has run in the game.

## The report

LARecomp (https://github.com/mzzvxm/LARecomp, commit `7b9726e`) says that with the 30 Hz pacing of the UI movies (`mcla_ui_movie_advance`, the same hook as ours: their code descends from this fork's) the race finish screens, the podium and the results cutscenes run in slow motion. In that commit they:

- turn the pacing off by default (`mcla_ui_timing_fix` false);
- when it is on, leave every movie whose name contains `HUD`, `RACE`, `POST`, `RESULT`, `PODIUM`, `CINE` or `END` unpaced;
- add a list `ui_movie_pacing` (default `PHOTOALBUMMOVIE`) of movies that are always paced.

Their commit does not name the movie that is slow, and gives no numbers. We have not seen it ourselves: nobody has run this fork through a finish line with the logging below. Here the pacing has been on by default since v0.2.x (`mcla_ui_timing_fix`), and since v0.3.1 Smooth UI (`mcla_ui_interpolate`, on) steps every movie 30 times a second. If the report is true for this build it ships in v0.3.1.

## What the code says (read, not run)

- The UI manager advances its movies in the Flash player's one-frame mode (`sub_825EDC08` with its flag set): one movie frame per call, whatever time has passed. Our hook holds a call back when the movie is not due. In the other mode (the flag clear) the movie advances by the elapsed time, and the hook leaves it alone.
- The engine timer gives the game the real elapsed time at any frame rate (a frame is clamped at 0.125 s), so the game's clock runs at wall speed. Paced movies step 30 times a second of wall time. On the console both were 30 updates a second. So by the code a movie that the game waits for takes as long as on the console, and the pacing cannot make it slower, unless something in the finish sequence runs at another rate than 30 steps a second on the console (see the table below), or the sequence is not made of movies the UI manager steps (a scripted camera, a video, a movie the game creates itself, as the HUD's gauge movie).
- The engine timer has a fixed-step field (`[timer + 32]`, loaded as the step of the original timing; believed to be 1/30 s on the console, not read live yet). If the finish scenes set another one (1/60 s), the movies there were made for another rate and 30 would be too slow for them. The log below shows it.

## What was added

`src/frame_timing.cpp`, `src/dev_pad.cpp` (nothing changes unless an option is set):

- `--mcla_ui_pace_log=true` (development): once a second one line, `ui pace: span_ms=.. updates=.. movies(name:ctx:swf_fps:calls/steps/dtcalls) NAME:CTX:FPS:C/S/D ... | timer dt= rate= mode_t= total_t= total2_t= step= scale= frames= state=`. `calls` is how often the engine asked to advance the movie in that window, `steps` how often it was let through, `dtcalls` the asks in the elapsed-time mode. The timer fields are floats read from the engine timer (`0x827D7500`: `dt` +8, `rate` +12, `mode_t` +16, `total_t` +20, `total2_t` +24, `step` +32, `scale` +84, `frames` +44) and `state` is the game state word (`0x8288D000`). `HUDGAUGE` is the gauge movie of the HUD (not in the UI manager's map). It works with the timing fix off (then every ask is a step).
- `--mcla_ui_movie_rates=NAME=RATE,...` (development, empty by default, **never run**): steps a second for named movies in place of the usual rule; `0` is every update, `game` is the inverse of the engine timer's step, a trailing `*` matches a prefix. It is the precise rule a fix would use (a rate per movie name, working with Smooth UI, which then glides over whatever step period that gives); it exists so that a follow-up chain needs no new build. If the measurement finds nothing, remove it.
- dev pad: the wait condition `follow_done` (the follower has stopped: the route ended).
- `launcher/dev/race_finish.py`: the pad (`pad`), the per-run report (`report`) and the comparison against a reference run (`compare`).

## The measurement

Pad (`race_finish.py pad`): `boot-to-race.pad` (Arcade, Ordered race, Sunset and Vine, 7 opponents, easiest, rain, midnight), then `follow sunset-and-vine.route` until its end (`mark route_end`; the route is the path the leading opponent drove, 4,440 m, 169 s), then the throttle held and 70 s of waiting with a picture (30% scale) every 5 s. **Nothing is pressed after the route**, so the sequence that follows is what the game does by itself (a results screen that waits for a press shows as a movie that stays). Not known before the run: whether the route's last point is past the finish line (its last speeds fall from 23 to 5 m/s, which looks like a car that has finished and is braking).

Runs (RTX 5080, Direct3D 12, 1x, HDR off, fast intro, a fresh copy of the save and the same seeded shader storage for each; all with `--mcla_ui_pace_log=true`):

| Tag | Cap | Option | What it is |
|---|---|---|---|
| `c30` | 30 | defaults | the reference: the console's rate, one update one step |
| `c120` | 120 | defaults | (c) Smooth UI on, every movie 30 steps a second |
| `c120-off` | 120 | `--mcla_ui_timing_fix=false` | (b) no pacing: every movie one step per update (4 times the console's speed) |
| `c120-v030` | 120 | `--mcla_ui_interpolate=false --mcla_ui_advance_on_input=false` | (a) v0.3.0's behaviour: screens 30 a second, HUD 60 |
| `c60` | 60 | defaults | Smooth UI at 60 |

Each run takes about 6 minutes (boot 1.5, race 3.5, finish 1.2), so the hold fits four or five of them; a run that does not fit is listed as skipped, in the order above. **Not run:** cap 60 with v0.3.0's behaviour, Vulkan, a slower GPU, uncapped, Linux, the PS5 build (the code is shared; not built or run there).

## Reading the result

`C:\mcv\racefinish\out\rf-summary.txt`: one report per run and the comparisons with `c30`. Redo it without the game: `python -I launcher\dev\race_finish.py report <run folder>` and `compare <c30 folder> <run folders>`.

- Per movie that first appears in the finish: how long it is on screen. Same length as in `c30` (within 15% or 1 s) means the same speed. **SLOWER** is a movie that stays more than 15% and 1 s longer than in `c30`.
- The timer's clock per wall second after the route's end (1.0 = the game runs at wall speed). **SLOWER CLOCK** is under 85% of the reference's: then the slow motion is the engine's (a time scale, or the 0.125 s clamp on a slow frame), not the movies'.
- `step-not-30`: the engine timer's own step in the finish is not 1/30 s.

What each result means:

| Result | Meaning | Next step |
|---|---|---|
| nothing SLOWER in `c120`, `c120-v030`, `c60`; the movies on screen as long as in `c30` | not reproduced in this build | answer LARecomp's report with the numbers (post nothing without the owner's say) |
| SLOWER in `c120` and not in `c120-off` | the pacing is the cause, for the named movies | find what the movie does at the console's rate; try `--mcla_ui_movie_rates` for it; do not just leave it unpaced (that is 4 times the speed at 120 FPS) |
| SLOWER in `c120` and in `c120-off` | not the pacing | the sequence is slow for another reason; look at the clock and the frame rate |
| SLOWER CLOCK | the engine's clock | the game's own slow motion at the finish, also on the console, or frames over 125 ms; look at fps in the report |
| `step-not-30` | the finish scenes run at another rate | try `--mcla_ui_movie_rates=*=game` |
| the route did not end | the run says so | the car did not reach the end of the route in the pad's wait: no finish part |

What the log cannot show: a movie that the game does not step through the UI manager's advance (a video, a scripted camera), and how it looks. The pictures (`out\rf-<tag>\shots\f00.png` ...) are for the eyes.

## Chain

One exclusive hold, `--max-min 35`: `C:\mcv\racefinish\chain\chain.ps1` (protected files before and after, the build of `fix/race-finish-pacing`, the runs of `rf_batch.py`). The queue command is in `C:\mcv\racefinish\NOTES.md`.

## Measured

Nothing yet.

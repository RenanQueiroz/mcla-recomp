# The wake on demand's audio watchdog: false trips at shutdown, and what a trip at start-up should do (2026-10-28)

`mcla_pump_wake_on_demand` (`src/poll_wait.cpp`) has an audio watchdog. The pump thread checks the SDK's steady underrun counter (`rex::audio::dump::SteadyUnderruns()`,
silent output frames after the game's first second of audio) once a round. A rise within two seconds of a round that came early used to turn the option off for the rest of the
session, with one log line: `pump wake on demand: the audio output ran dry (N silent frames) while the thread was coming round early; ...`.

**State: the change below is compiled by CI only. It has not been run in the game.** What was read and what was measured is marked.

## What the logs say (measured: read from existing game logs; `wd_survey.py`, `wd_startup.py`)

Every `game.log` of the chains in the scratchpad that has the line `pump wake on demand: on.` (554 logs, v0.2.0 to v0.3.0 builds, 4 to 16 logical processors, all on the
same PC) was read. 392 have no trip. 161 have one (the first of the log is counted; a log that holds several sessions counts once):

| Where the trip is | Logs | Detail |
|---|---|---|
| After `Window closing, shutting down...` | 128 (127 of them 0.04 to 0.07 s after it) | The shutdown: `TerminateTitle` stops the guest threads, the audio worker stops feeding the output, the SDL callback plays silence, and the pump thread's next round sees the counter rise. In the log it sits between `Execution complete` and `Title terminated; hard-exiting process.` |
| In the run | 33 | 28 within 20 s of the option starting, 4 between 20 and 60 s, 1 at 135 s |

The one outlier of the first row (820 s) is a log of three sessions appended; its third session has a real trip in the run, so the table is off by one at most.

The 33 in-run trips sit at the game's loads, judging by the last pad line before each (inferred; the logs do not name a cause): the intro (the pad has only started), the title, the
start of the city load (`mark map_zoom`; the c3w0 trip came 22 ms after the pad saw the title left), a settings change that rebuilds pipelines (`set mcla_anti_aliasing`). The
silences are 1 to 15 frames.

Does the option cause them? Not shown either way, but there is this: of the 103 logs that have `audio stats` lines, 53 never had the option on, and 3 of those had steady
underruns in the first 40 s of audio without it (574 frames in `c3ab4` off r1, the first run of its batch, 3 frames in `e4-ship`, 1 in `u13c4` off r1). With the option on and not
yet tripped, 951 two-second intervals in the first 40 s of audio had none, and the 1,224 later ones had 3 (the deliberate test trip). The logs with audio stats are mostly warm
A/B runs, the 33 trips mostly cold pad runs, so this is not a controlled comparison.

## What the watchdog does now (inferred from the above, not tested in the game)

1. **Shutdown.** Silence after the game's shutdown has begun is not counted. The signal is `KernelState::is_terminating_title()` (the SDK sets it at the start of `TerminateTitle`
   for every way the title can end), latched when first seen because the SDK clears it before the process exits, and set earlier by the fork's own paths: the window's close request
   (`MclaApp::OnWindowCloseRequested`, after the save wait), the end of the guest's main thread (`MclaApp::OnGuestThreadExit`), the in-game QUIT/RESTART (`CloseGame`) and the dev
   pad's quit. The pad's `close` posts `WM_CLOSE` and takes the first path.
2. **A trip in the run pauses the option, the second one turns it off.** The first silence goes back to the game's own Sleep for `mcla_pump_wake_pause_s` seconds (60), logged
   (`... goes back to the game's own Sleep for 60 s and then comes round on demand again; a second silence of that kind turns it off ...`), then logs `pump wake on demand: back on
   after the pause` and carries on; the silences of the pause are not charged to it (the counter is read again when it ends, and the last early wake forgotten). A second trip, or
   `mcla_pump_wake_pause_s=0`, is the old behaviour: off for the session, with the old line. Why not latch at the first: a stall while the game starts or loads is the likely reason
   (above), and the option is worth about +39 to 51% FPS uncapped at 4 logical processors (2 runs a side). Why not ignore the first seconds: the 33 trips are not only at the start
   (loads happen at any time), and a silence is a gap in the music. Cost if the option was the cause: one more gap of a few frames after the pause, then it is off.
3. **The development trip** `mcla_pump_wake_test_trip_s=N` now trips N seconds after the first early wake and again N seconds later for every trip so far, so one run with a short
   `mcla_pump_wake_pause_s` shows the pause, the way back and the way off.

## What a run should show (for whoever measures it)

- No `ran dry` line within a second of `Window closing`. A line there means a path to shutdown that nothing marked (look at the lines just before it).
- After a trip in the run: the pause line, 60 s later the `back on` line, and `pump wake on demand: ... early wakes` counts that grow again in the `poll tick` lines.
- The sound around a pause was not listened to.

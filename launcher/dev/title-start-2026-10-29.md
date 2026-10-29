# Start has to be pressed many times on the title screen (2026-10-29)

Owner's report (Xbox Series controller and DualSense, both through SDL): "I have to press Start multiple times till it's finally detected." The attract video that plays after a while of idling also took many presses to stop, and it happens after the game has run for a while.

**Status: cause found (read in the second round's logs, no new run needed); fix written, off by default, not measured yet (`mcla_script_pad_latch_ms`, `src/pad_probe.cpp`).** The third round's chain (`C:\mcv\title3\chain`, not in the repo) measures it; the numbers go into the "Measured" section below when it has run.

## The cause

The title screen and the attract video are not decided by the menus' key path. A game **script thread** polls the pad with the native `IS_PAD_BUTTON_DOWN` (`sub_822C8AF8`; the name is in the native table registered by `sub_822CA238`) and that native is an **edge test**: `((cur xor last) and (1 << button)) and cur` on the pad record (`0x82905388 + 64 * pad`, `cur` at +8, `last` at +12). It is true for ONE game update only, the update in which the pad record changes. The thread (it also calls `Game_FadeLoadscreenLogo`, `IsChildFinished` and the wait natives) polls 8 buttons a loop, Start (button 11) among them, and runs its loop only every **3rd update at 120 FPS, every 2nd at 60 and every update at 30** (the `script natives:` part of the 1 s lines: `Game_FadeLoadscreenLogo` x40 a second at 121 updates, x30 at 61 and at 31). So a press is seen only when the update the pad edge falls in is also an update in which that thread runs: about 1 press in 3 at 120 FPS, 1 in 2 at 60, every press at 30.

Evidence, `C:\mcv\title3\tools\edgecheck2.py` over the 24 runs of the second round (69 Start presses; per press: did the thread call `IS_PAD_BUTTON_DOWN` in the update of the pad edge, and did `MoviePlayer_StopMovie` (`sub_822CC808`) follow within 45 updates):

| Thread ran in the press's update | StopMovie followed | Presses |
|---|---|---|
| yes | yes | 25 |
| no (it ran 1 to 5 updates later) | no | 27 |
| no read at all in the window (after the title was left, the attract video's fade) | no | 16 |
| no | yes | 1 (`lim-c120-a`, 4 logical processors, the 200-native log limit) |

Every press the thread's loop shared an update with was accepted (the title exits 3 updates later: `StopMovie`, `IsChildFinished`; the attract video stops about 40 updates later, a fade); no press that came in an update without a loop run was. Examples: `t120-b` press at update 480, loop at +0, StopMovie 483 (accepted at the first press); `t120-a` presses at 449 (loop at +2), 629 (+2), 810 (+1), 990 (+1), 1170 (+1), 1351 (+0, accepted: the sixth). At a 30 FPS cap (`t30-a`) and with `mcla_title_fps=30` (`c*`) the loop ran in every update, so all 10 first presses were accepted.

This explains what the first two rounds measured:
- per press about 0.35 chance at 60 FPS and above and no memory (a single press at 120 FPS was ignored for 20 s), the same whatever came before, and the same at 60, 120 and 240 FPS (the thread runs every 2nd to 4th update, a share of 1/2 to 1/3 whatever the rate: not a "check every 250 ms");
- the menus' UI input object logs a key down for Start at every press and the scene stack takes it (`mcHUDForm`'s form key handler answers 1) but the title does not use it: nothing about Start is decided on that path;
- a longer press helped only by chance: `mcla_title_pad_hold_ms` (round 2; removed) kept `cur` set for 300 ms but left the edge at one update, and measured 2, 1 and 1 presses;
- `mcla_title_fps=30` works for the same reason (the thread runs every update at 30 updates a second);
- the 42 runs of the first round that always needed 9 presses were load time after an accepted press on 4 logical processors at a 30 FPS cap, not ignored presses (they counted presses made during the load);
- the first round's `mcla_title_edge_hold_ms` hooked the wrong native (`IS_BUTTON_DOWN`, `sub_822C8AB0` -> `sub_822C7FC8`, which the title never calls: `native_calls=0` in every run);
- the old "accepted" detector (`EVENT_START_SCREEN_EXIT`, `sub_821FAD78` at `0x821FAE10`) never fired because the title's statechart event does not arrive through that handler (the compare and the hook address are right; the event comes some other way, a script posting it by name or data). The report now says SAW / MISSED from the script's own read.

The attract video (`idle120-p`, `idle120-t`, shots in `C:\mcv\title2\out\shots`): the video starts at update 13968 (115.4 s). Press 1 (update 14404) missed (loop at +1), press 2 (14585) hit (loop at +0) and `StopMovie` came at 14625, 40 updates later (the shot after press 2 is the fade to black, the next the title with its logo again), presses 3 to 10 missed (press 3 at 14765 had no read at all: the thread does not poll for about a second after the video stops), press 11 (16211) hit and left the title (`StopMovie` 16214). In `idle120-t` press 6 stopped the video and press 10 left the title. So the earlier reading "nothing for 15 s" was wrong: the video stops on the first press that hits the loop; the title needs another. No minimum play time shows (a press 3.6 s into the video missed by phase, one 5.1 s into it stopped it); earlier than that was not pressed (`attract-f` in the chain does).

Why the thread runs only every 2nd to 4th update is **not known**. `sub_82553C10` is `scrThread::Update` (adds 16 ms, `1/60 s` as an integer, to the thread's two clocks per call and calls `scrThread::Run`, `sub_82554E20`); the wait natives (`sub_82553DD0`, `sub_82553D30`) add 16.67 ms per poll to the thread's wait clock. The thread's loop probably waits a couple of those polls. The chain's trace (`TS thread run` and `TS thread update` lines for the threads that poll the pad) shows how often `Update` and `Run` are called per game update; `o-a` and `o-b` (original timing, `--mcla_fps=-1`, the game's fixed 30 Hz step, vsync on) say whether the cadence follows the step (every update in the console's mode, as at a 30 FPS cap) or the clock. It does not matter for the fix.

## The fix (`mcla_script_pad_latch_ms`, default 0 = off; docs/off-switches.md)

Hook `mcla_script_pad_read` at `0x822C8B34` (the `cntlzw r6,r8` in `IS_PAD_BUTTON_DOWN`: r8 = the press, r6 = pad, r5 = button) and the pad update hook (`0x82615048`, counts each press: a sequence number and the time per pad and button). A script **call site** that is not given the real press is told about it **once**, at its next poll, when (a) it polled this button within 200 ms before the press (it is a poller; a call site that starts asking after the press is not told, so one press does not also answer the next screen), (b) it has not been told about this press, (c) the press is at most `mcla_script_pad_latch_ms` old (100 ms is what the chain tests: two to four polls of the title thread) and (d) the title phase is on (the guard at `0x82836A28`, bit 0, is 0). A call site that read the real edge is marked as told; later reads of the same update answer the same. A call site is the running thread's program (`[thread + 8]`) and program counter (`[thread + 16]`), the thread being the guest word at `0x828DA3BC`, which `scrThread::Run` sets: it does not matter whether the script keeps one thread or starts a new one for each pass of its loop (not known). No pad hold, no change to the UI path. The result is what the console does at 30 FPS: every script that polls a button sees each press once.

Risks: it applies only while the title phase lasts (the readers after it, the frontend's and the city's scripts, are untouched; if they have the same problem at 120 FPS it is not measured and not fixed); a press made while the title thread does not poll (the second after the attract video stops) is lost, as on the console; a hold of the pad record is not needed.

## Measured (second round, before the fix; the numbers behind the table above are in `C:\mcv\title2\out\ts-summary.txt`)

| Cap | Presses until Start was accepted (one number per run) | Accepted / pressed |
|---|---|---|
| 30 | 1, 1, 1 (also a single press) | 3 / 3 |
| 60 | 3, 2 | 2 / 5 |
| 120 | 1, 3, 2 (first round); 3, 6, 1 (second round) | about 1 in 3 |
| uncapped (about 240 updates a second) | 3 | 1 / 3 |
| `mcla_title_fps=30` at 120, 60 and uncapped | 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 (all pressed at the same update, 140 to 142) | 10 / 10 |
| `mcla_title_pad_hold_ms=300` (removed) | 2, 1, 1 (50 ms press) | per press as without |

## Measured (third round)

Not run yet when this was written. The chain (`C:\mcv\title3\chain\ts_batch.py`) runs: `b120-a`, `b120-b`, `b60-a`, `b0-a` (no fix, the trace of the script side: loop run in the press update or not, SAW / MISSED per press), `o-a`, `o-b` (original timing), `f120-a..e`, `f60-a/b`, `f0-a..c`, `f120-h50` (the latch, first press accepted?), `lim-f120` (4 logical processors), `idle-f` (16 presses 1.5 s apart in the attract video), `attract-f` (presses 400 ms apart from 0.5 s after the video starts). To turn the latch on by default: the first press SAW in every `f*` run at 60, 120 and uncapped (also the 50 ms press), the video stopped by the first press in `idle-f` and no second step on the screen after the title.

## Not done / not built

- Linux and the PS5 build: not built or run. `src/pad_probe.cpp` and `config/pad_probe.toml` are shared with both; the hook addresses are those of the Xbox 360 image.
- The hooks' codegen and behaviour: nothing of the third round ran. CI compiles only the host source.
- The same polling may exist in the frontend's and the city's scripts (`IS_PAD_BUTTON_DOWN` has many readers in the image); not checked.

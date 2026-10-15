# A race benchmark (2026-10-15, D3D12, RTX 5080)

A scripted segment with AI opponents and traffic, heavier on the CPU than the
solo drive (`drive-spawn.pad`), for the D3D12 work on EDRAM transfers,
resolves and texture loads.

## The race

- **Menu path:** from the city, Start opens the pause menu. RB goes to the Modes tab;
  Arcade, Ordered race and "Sunset and Vine" (Hollywood) are the first item of
  each list.
- **Options:** the race options start at the arcade defaults. `boot-to-race.pad`
  sets 7 opponents, one step easier than Medium (shown as "EASIEST"), power
  ups off, normal traffic, Rainy and Midnight, then starts the race.
- **Easier opponents:** with the default Medium the pack leaves the player out of sight
  within seconds; on the easiest setting they stay around the player for
  the whole segment.
- **Driving:** `follow sunset-and-vine.route` drives the player's car. It is a new
  command in `src/dev_pad.cpp`: pure pursuit on the stick and route speeds
  on the triggers, run on the game's update thread. The route is the path
  the leading opponent drove (4,440 m in 169 s), recorded with `route.py
  record` over the pipe's new `cars` command.
- **Pop-ups:** the follower presses A when the whole world stands still, which
  dismisses the tutorial pop-ups ("Slip Stream Turbo", CONTINUE (A)). A is
  the handbrake in a race.
- **Timing (`race.pad`):** the `race` segment starts when the race starts, at
  9.3 s on the mode timer at 0x827D7510 (game time since the race loaded),
  and lasts 90 s of game time (5,400 updates). `watch 60` logs every car's
  position once a second of that timer, and `devrun.py bench` compares
  runs with it.
- **Timedemo menus:** under `--mcla_fixed_dt` the menus get two fixes:
  - Presses last 4 updates. A held direction repeats after about 8 updates,
    so 70 ms presses counted twice when the game ran uncapped.
  - The pause menu opens right after arriving in the city. The career's
    phone message, 10-20 s of game time in, put the presses out of step.
- **Replay:** the race pause menu has "View Replay". It replays the
  current race with chase and cinematic cameras, frame advance and pause.
  Its own pause menu offers Continue, Exit, Restart replay and Save game.
  There is no way to save a replay or load one in a later session, so it
  cannot serve as a benchmark across runs.

Run it:

    python launcher\dev\devrun.py bench --script launcher\dev\scripts\race.pad --configs d3d12:1,d3d12:3 --runs 5 --fixed-dt 60 --track --name race

Segments are `title`, `map_zoom`, `menu`, `race_intro` and `race`. A run takes about
80 s at Native and 115 s at 3x.

## Repeatability

The race is not deterministic from run to run, even as a timedemo:

- **Cars:** the `watch` lines part by about 1 cm during the intro, before the
  start, and are 70-170 m apart 27 s in. AI or physics state differs from
  the race load on.
- **Player's path:** the follower holds the player's car on the route (largest
  distance from it 5.1-8.6 m per run). After 90 s it reaches different
  points: 512-565 of 1,111.
- **Pop-ups:** 0-3 tutorial pop-ups per run, each a fraction of a second of
  frozen world.
- **Per-run medians:** the race medians vary 5.7-5.8% between runs, against
  1.3-2.5% for the solo drive. The mean varies 5.0-5.6% and the 90th
  percentile 2.9% at 3x.
- **The other segments:** `menu` varies 0.4-1.4%, `race_intro` 1.4% at
  Native and 15% at 3x, `map_zoom` 0.9% and 8.6%.

Compare median-of-5 runs, alternating configurations. An effect under about
3% needs more runs or an in-run A/B (`ab-harness.patch`).

Medians in 10 s windows of game time show where the race is heavy:

- At Native, the first 40 s through Hollywood with the pack around the car
  take 7.0-8.9 ms.
- At Native, the rest sits at the game thread's limit, about 6.8 ms
  (146 FPS).
- At 3x, the windows go from 13.5 ms at the start to 11.4-12.9 ms later.

## Baseline (D3D12, launcher defaults)

Timedemo (`--fixed-dt 60`), uncapped and windowed. Five runs per configuration after
one warm-up, alternating Native and 3x.

| `race` segment | Native | 3x |
|---|---|---|
| Median frame (runs) | 7.11 ms (7.09, 7.11, 7.37, 7.01, 7.42) | 12.70 ms (12.30, 13.03, 12.78, 12.70, 12.40) |
| Mean | 136 FPS | 78 FPS |
| 1% low | 51 FPS | 40 FPS |
| Worst frame per run | 79-116 ms | 61-74 ms |
| Segment length (real time) | 39.7 s | 69.3 s |

For comparison, the solo drive in the same timedemo mode measured:
- 6.83 ms at Native (1% low 113 FPS);
- 13.96 ms at 3x (1% low 51 FPS), in `vulkan-2026-10-14.md`.

The race is harder on the CPU: the median is 4% higher at Native and the 1% lows are
much lower. Its median at 3x is lower than the sunset drive's: midnight is
cheaper for the GPU than the drive's dusk at the spawn point.

## Where the GPU time goes at 3x in the race

Measured with `gpu-timestamps.sdk.patch` (applied, built, measured and reversed):
- 2 runs with `MCLA_GT_EVERY=2`;
- 65 timed frames from inside the `race` segment;
- `gt-analyze.py`.

| Share of the GPU frame | Race, 3x (13.3 ms median) | Title-screen city, 3x (12.0 ms, `profiling-2026-10-14.md`) |
|---|---|---|
| Game draws | **46.5%** (6.3 ms) | 32% |
| EDRAM ownership transfers | **24.6%** (3.3 ms) | 32% |
| Resolves | **13.2%** (1.7 ms) | 17% |
| Texture loads | **9.5%** (1.3 ms) | 15% |
| Uploads | 5.6% (0.7 ms) | 4% |

Emulation work is 53% of the race frame (7.0 ms), against 68% on the title
screen. The race draws about twice as much: around 4,000-6,000 draws a frame.
The transfers and resolves still cost 5 ms.

Notable passes in a median frame (12.66 ms):
- **Main scene:** drawn twice into the same 2xMSAA `2_10_10_10_FLOAT` +
  `D24FS8` targets, 1,865 draws each: 3.42 ms (resolves 0.45) and 1.70 ms.
- **Single-draw MSAA switches:** one-draw 4xMSAA passes on the main scene's
  EDRAM range cost mostly transfers: 0.45 ms (0.41 transfer), 0.17 ms and
  0.22 ms (0.21). This is item 3's target, as on the title screen.
- **Two 4xMSAA `8_8_8_8` passes** (18 and 97 draws) cost 0.74 + 0.81 ms, of
  which 0.83 ms is transfers and 0.46 ms resolves. They did not show on the
  title screen.
- **HUD and final target:** the 1x `8_8_8_8` target at EDRAM base 720 takes
  0.55 ms (0.29 transfers). The 4xMSAA depth-only draw after it takes 0.21 ms
  of transfers.

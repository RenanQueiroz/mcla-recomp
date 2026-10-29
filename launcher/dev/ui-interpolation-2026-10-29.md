# Interpolated UI: a spike (2026-10-29)

Question from the owner: the menus, pop-ups, pause menu, map panels and the HUD (gauges, song ticker, "LOADING" badge) are Flash movies that advance one movie frame per step. The fork steps the screens 30 times a second and the HUD at `mcla_ui_hud_rate`. Above 30 FPS the menus look stepped, and raising a rate makes the animations faster. The owner does not like the rate settings and wants the UI smooth at any frame rate without speeding anything up.

**Everything below is read from the recompiled game and the unpacked image, not run.** The prototype is written, off by default, and was compile-checked by CI only; the check chain has not been run. Where a number is computed or inferred it says so.

## Answer

Practical: yes, with one hook in the player's draw walk and a few bytes of bookkeeping at the step hook that exists already. The player is RAGE's own `rage::swf*` player (not Scaleform). Every node of a movie's display tree has a local matrix and a local colour transform that the step writes, and the draw walk computes world values from them once per drawn frame. A hook right after that computation can redo the world values from local values blended between the last two steps, for every node, without touching what the step or the game's scripts see. Nodes that appear, change character or jump are shown as the step left them. Nothing in the player has to be rewritten.

What it cannot do: smooth anything that is not a matrix or colour-transform change between two steps (a needle drawn as a different shape on each frame of a timeline, text scrolled inside its field, morph shapes), and it shows the picture one step (up to 33 ms for the screens) behind the game's state. How much of the UI falls in the first group is not known until the check chain has run: it needs the movies' content, which is in the encrypted archives, so the chain counts it from the running game (the "census" below).

## What the player is (guest addresses; offsets in bytes)

RTTI names the classes: `swfCONTEXT`, `swfFILE`, `swfSPRITE`, `swfINSTANCE`, `swfSHAPE`, `swfTEXT`, `swfEDITTEXT`, `swfFONT`, `swfMORPHSHAPE`, `swfBITMAP`, `swfBUTTON`, `swfCMD_PlaceObject2` and others (`rage::`). Scaleform strings do not occur in the image.

| What | Where |
|---|---|
| `swfCONTEXT` vtable | 0x820852FC: +0 destructor 0x825EEFD8, +4 **Advance 0x825EDC08**, +8 **Draw 0x825EE970** |
| `swfINSTANCE` vtable | 0x82086E0C (a node of the display tree: every placed shape, text, sprite) |
| UI manager's wrapper of a movie (`movie+56` = the `swfCONTEXT`) | step: 0x82720898 (calls the context's vtable +4, then 0x82724010); draw: 0x82720908 (render state, then the context's vtable +8); dispatcher by pass: 0x82721258 |
| Who steps | the UI manager's update 0x821FC588 (all screens, in one-frame mode), the 3D icons 0x82225030 (time mode), `mcHudManager`'s update 0x8223D648 (vtable 0x82029490, RTTI `mcHudManager`: two arrays of movies, stepped in one-frame mode; the HUD's movies) |
| Who draws | `mcUIClass` render callback 0x82227428 (vtable slot) -> `mcUIManager::Render` 0x821FB1C8 -> 0x82721258 -> 0x82720908 -> 0x825EE970 -> 0x825EE428 -> **0x825FB348** (the walk) |
| Exclusion of step and draw | `FlashSemaWait` 0x82305870 / `FlashSemaSignal` 0x823058F8 around both 0x821FC588 and 0x821FB1C8 (the string is in the image: `mcRenderThread::FlashSemaWait('%s')`) |
| The context being stepped or drawn | the word at 0x828FF970 (written by 0x825EDC08 and 0x825EE970, cleared after) |

`swfCONTEXT` (the `ctx` of the hooks): +24 float accumulator, +28 the `swfFILE` (u16 at +48 of it = the movie's frame rate in frames a second; not read yet, the chain logs it), +32 the root instance, +36 a pointer the draw passes down.

**The step.** 0x825EDC08(ctx, f1 = dt, r5 = one-frame flag, r6): frame time = 1.0 / the file's frame rate. With r5 != 0 (the UI manager passes the byte at 0x827DC1FC, which is 1 and never written): accumulator := frame time, exactly one step, accumulator := 0. With r5 == 0 (the 3D icons): accumulator += dt, one step per whole frame time, **the remainder stays in ctx+24** (that is a free interpolation fraction for those three movies). A step is `0x825FB0D8(instance)`: the instance's handler at +216, `def->vtable[4](instance)` (for a sprite 0x825FDB30: it runs the current frame's list of `swfCMD`s; `PlaceObject2` is 0x8260B038), the handler at +220, then the children (first child +188, next sibling +192, parent +184). `PlaceObject2` finds or creates the instance at a depth (0x825FC5E0: it **reuses the instance at that depth** and only stores the new character definition at +4), sets its local matrix with 0x8260AAD8 (which also decomposes the matrix into rotation, scale and position fields and sets a dirty flag at +81) and copies 8 bytes of colour transform to +56.

**A node** (`swfINSTANCE`, 224 bytes from a pool, 0x825FC410):

| Offset | Content |
|---|---|
| +4 | character definition (`swfSHAPE`, `swfTEXT`, `swfEDITTEXT`, `swfSPRITE`, ...) |
| +8..+28 | **local matrix**, six floats: a b c d tx ty (rows (a b), (c d), (tx ty); world = local x parent). Also the first part of the 148-byte "state block" that is passed down: +0..+20 local, +24..+44 world, +48..+55 local cxform, +56..+63 world cxform, +68..+71 tint RGBA (255), +72 a flag |
| +32..+52 | **world matrix**, written by every draw |
| +56..+63 | **local colour transform**: multiply RGBA (signed bytes, 64 = 1.0), add RGBA |
| +64..+71 | **world colour transform**, written by every draw |
| +80, +81 | a flag copied from the parent; the "script changed _x/_y/_rotation/_scale" flag: the local matrix is rebuilt from the properties at draw time by 0x825F8C70 |
| +152 | clip depth (masks), -1 for none; +164 current frame; +166 depth (u16); +168 character id (u16); +177 active; +67 visible/type; +184/+188/+192 parent/child/sibling |

**The draw walk**, 0x825FB348(node r3, parent's state block r4): returns at once if the node is not active (+177); rebuilds the local matrix if +81 (0x825F8C70); computes the world matrix (0x825FB38C..0x825FB454: W0 = l0 p0 + l1 p2, W1 = l0 p1 + l1 p3, W2 = p0 l2 + p2 l3, W3 = p3 l3 + p1 l2, W4 = p0 l4 + p2 l5 + p4, W5 = p3 l5 + p1 l4 + p5, every product rounded to single, the parent's p at r4+24..+44); combines the colour transform with `0x825F8B20(dst = node+64, local = node+56, parent world = r4+56, tint)` at 0x825FB480 (per channel: add = (local add x parent mult >> 6) + parent add; mult = ((local mult x parent mult >> 6) as a byte x tint / 255); then, at 0x825FB484, nothing more is needed from the node's local values. It sets up masks (0x825ED5E0), then walks the children (the loop at 0x825FB554) or, for a leaf, calls `def->vtable[3](def, state block, mode)` (shapes: 0x82608548, which copies the world matrix at +24 of the block into the renderer's state; text: 0x82603BB8). Mask nodes go through the same walk. The walk is the only caller of itself and of the draw path (0x825EE428 is its only entry).

## The hook

`mcla_ui_interp_draw` at **0x825FB484** (`lhz r9,170(r30)`, right after `bl 0x825F8B20` returns), registers r30 = node and r31 = parent's state block, in `config/frame_timing.toml`. For each node of a movie that the step hook paces:

1. The node's local values (matrix at +8, colour transform at +56) are read. A table keyed by the node's address (and checked against its definition, depth, character id and parent) keeps, per node, the blend's start values (`from`, what was shown when the step arrived), the target (`to` = what the step wrote), and what was shown last.
2. If the live values differ from `to`, a step (or something else) changed the node: a new glide starts from what was shown (so the picture never jumps back), unless the node is new, was another character, was not drawn during the last step (nothing to glide from), moved farther than `mcla_ui_interp_snap_px` (400) or changed scale or rotation by more than 0.75 in a matrix element (a cut, a wrap, a flip): then it is shown at once.
3. The share of the step period that has passed since the step (`mcla_ui_interp_window_pct`, 100) gives the blend; the node's world matrix and colour transform are recomputed from the blended local values and the parent's world values (already blended: the parent's hook ran before it walked the children) with the player's own arithmetic, and written to +32..+52 and +64..+71 only if they differ from what the player computed.

Nothing else is written: the local values stay the step's, so scripts and the next step see what they always saw, and the draw hook writes only fields that the draw walk writes itself (the world values), so it adds no new access from another thread to anything the update thread writes. The step hook (`mcla_ui_movie_advance`) notes, for each paced context, when its step belongs on its schedule (the ideal time, not the update's: with the 30 a second schedule and a 100 FPS cap the steps then still glide evenly), the nominal period, and a step counter. In one-frame mode a call that passes the hook always steps exactly once.

Three details that matter:

- **One time for a whole drawn frame** (the update counter of the context and the time of its first node): a mask and what it masks, or two passes that draw the same movie, must not glide a fraction of a pixel apart.
- **Nothing is done while the update time is longer than 0.6 of a step** (30 FPS and below for the screens at 30 a second; 60 FPS and below for the HUD at 60): there is no gain with fewer than two frames per step, only delay. The update time is smoothed over the calls of the step hook.
- **Static nodes cost a hash lookup, a mutex and about thirty guest reads** per node per drawn frame. Not measured; computed from the operations, a few hundred nodes at well under a millisecond a frame. The draw walk of the movies runs anyway every drawn frame, so the option adds no draws.

## What the hook does with what the brief asked about

- **Matrices and colour transforms:** blended (matrix element by element, which is the right thing for the small per-step changes of an animation; the colour transform by bytes, which have 64 levels, so a fade is as fine as the player's own).
- **Appear, disappear, change character:** shown as the step left them. A node that disappears is gone at the step (the table entry ages out after 5 s); a node that appears, or a depth that gets another character (the instance is reused, the definition at +4 changes), is drawn at its final place at once, while its neighbours glide: up to one step of mismatch in time between structure and motion.
- **Masks:** nodes like any other; the mask and the masked content glide together because they share the frame's time.
- **Nested clips:** every node blends its own local values and gets its parent's already blended world values; the product of two blended values differs from the blend of the products by a second-order term (about the product of the two step deltas: negligible for steps of a few pixels).
- **Text:** the field's matrix glides; its content is the step's. A ticker that moves a text field's `_x` glides; one that scrolls text inside a fixed field (`hscroll`) does not. Which one the song ticker is is not known (the movie's scripts are in the archive).
- **Gauges and the ticker (HUD):** the game sets variables every update and the movie's scripts set `_rotation`, `_x`, or call `gotoAndStop` when the movie steps. `_x`, `_y`, `_rotation` and `_xscale` set the property fields and the dirty flag, and the matrix is rebuilt at draw time from them (0x825F8C70, before the hook): those glide. A needle that is a different shape on every frame of a timeline (the instance at the depth is reused with another character) cannot glide. The chain counts which it is per movie. With interpolation the HUD could run at 30 steps a second (the console's ticker speed) and still draw smoothly: that is the point for the HUD.
- **The 3D icons** (`3DICONSMOVIE0..2`, time mode: not stepped by the fork's hook, so left alone): their step remainder is in `ctx+24`, which would give an interpolation fraction without a clock; not used.
- **Minimap, GPS map camera:** moved by the game itself, not by movie steps: unaffected either way.

## Alternatives weighed

- **Fractional time in the player** (time mode, `r5 = 0`): the player accumulates the time and steps whole frames; no draw sees a fraction. It would only replace the fork's schedule by the player's own; it does not make anything smoother. The remainder in `ctx+24` is the one useful by-product (above).
- **Per-movie rates:** a higher rate is a faster animation, because the content is authored per frame: it can trade smoothness for speed, not give both. That is what the owner dislikes.
- **Only the transform of top-level clips:** fewer nodes, but the visible motion in menus (the highlight, rows, a number changing) is in nested clips; the hook is generic and cheap, so there is no reason for the restriction except a staged rollout (restrict to a movie by name, or a depth).
- **Extrapolation** (draw `to + (to - from) x alpha`): no added delay for a steady movement, but an onset from rest cannot be predicted (so a press, which is what the owner feels, gains nothing), a stop overshoots and bounces, and every cut needs a guard. Not recommended.
- **One step behind** (what is built): latency is at most one step period for the glide to complete, 33 ms at 30 a second (computed), less with `mcla_ui_interp_window_pct`. With `mcla_ui_advance_on_input` the step happens 0.2 ms after a press (measured earlier, `menu-lag-2026-10-29.md`), so the first visible movement comes within one frame of the press (8 ms at 120 FPS), not after a whole step; the highlight is at its new place one step later than a stepped display would have put it there at once. Whether that is felt is for the owner to try: it is the price, and the window option is the knob.

## Effort and risk (estimates; none measured)

- Prototype, as written: done (this branch). It is one hook of about 150 lines, a few bytes of bookkeeping at the step hook, a development log, a self-check and a check script. Not run.
- To ship in v0.3.1: run the chain (45 minutes, one hold); fix what it shows (snap rules, the nodes that cannot glide); look at the pictures of the pause menu, a pop-up, the transition screens, the title screen and the HUD at 120 FPS and 60 FPS, 1x and 3x; a settings row (pause menu and launcher, text, docs); then a real play session by the owner. About two to four days of work, with a real chance that the first chain's census says the HUD's needles do not glide and the owner has to decide whether the menus alone are enough.
- Risks, most likely first: (1) content that does not glide (per-frame shapes, scrolled text): unknown share; (2) the one-step delay is felt in fast menus (mitigation: the window option; a per-screen choice); (3) a mid-asm hook inside the draw walk is the first of its kind in this fork: the generated code must accept it (the chain's build step; the hooks of the key repeat are the same kind at other addresses) and the draw walk's cost grows by the hook (not measured); (4) the draw and step threads: the semaphore keeps them apart for the UI manager's movies; the HUD's movies are stepped by `mcHudManager`'s update (0x8223D648), and whether that call is inside the same semaphore was not read, but the hook only reads the local values (as the draw walk does) and writes the world values (which only the draw walk writes), so a step in the middle of a draw would not be made worse; (5) the tables grow with the nodes (a few hundred; entries age out); (6) a frame drawn in tiles (single-tile rendering is the fork's own): each tile pass draws the UI with the same frame time (one time per update), so tiles agree.
- Linux and PS5: `src/frame_timing.cpp` and `config/frame_timing.toml` are shared; the hook is plain C++ over guest memory with the same addresses (same game image). Not built, not run on either; the PS5 build was not considered further.

## The prototype

Options (all in `src/frame_timing.cpp`, rows in `docs/off-switches.md`):

| Option | Default | |
|---|---|---|
| `mcla_ui_interpolate` | false | the feature |
| `mcla_ui_interp_window_pct` | 100 (10 to 100) | the share of a step period over which a node glides |
| `mcla_ui_interp_snap_px` | 160 (was 400) | a node that moves farther than this in one step is not glided; one that turns round and jumps more than 40 px (and 8 times the step before) is not glided either |
| `mcla_ui_interp_log` | false | development: logs the gliding nodes, a census of each step, the nodes that snap or change character and a self-check; also works with the feature off, then it logs what would have been drawn |
| `mcla_ui_interp_stats` | false | development: once a second, the calls of the hook, their total and worst time (the time stamp counter around each call; cheap enough to measure frame times with) |

Log lines (with `mcla_ui_interp_log`): `ui interp: <screen> inst=<node> d=<depth> ser=<step> t=<ms> alpha=.. live=(x,y) shown=(x,y) world=(x,y) applied=0|1` for each gliding node of each drawn frame (live = the step's local translation, shown = what the blend gives, world = the node's drawn world translation); `ui interp step: <screen> ser=.. moved=.. cx=.. fresh=.. snapped=.. gap=.. max_move=..px tracked=..` for each step (the census: how many nodes moved, changed colour, were new or another character, were snapped, how large the biggest move was); `ui interp movie: <screen> ctx=.. swf_fps=.. period=.. us` once per movie; `ui interp check: nodes=.. matrix_off=.. (worst ..) cxform_off=.. gliding=.. applied=..` every 3 s: the player's own world values for every node compared with this code's arithmetic on the same local values (a mismatch would be an error in the formulas above; expected 0).

## The check chain

`launcher/dev/ui_interp.py` writes the pad and reads the log; the chain around it (build, run, protected-files check) lives outside the repository, in the agent's work folder `C:\mcv\uiinterp\chain` (`chain.ps1`, `run_interp.py`), and is queued by the main session (the command is in the agent's notes). One exclusive hold, built from the pushed branch, expected 30 to 40 minutes (build with code generation 3 to 6, the run 8 to 10).

The run (D3D12, native resolution, a scratch save): to the city; **hud** (HUD at 30 steps a second, cap 120: full throttle 5 s, braking 2.5 s; option off then on); **pause** (the Settings tab, cap 120, option off then on: 4 taps down and 4 up 600 ms apart, a hold of 1 s, 1.5 s at rest and a picture, then a burst of four pictures after a press); **open** (the pause menu closed and opened four times: off, on, off, on); **caps** (the taps again at caps 60 and 30 with the option on). The report says, in `C:\mcv\uiinterp\out\interp-report.txt`, `summary.txt` (PASS/CHECK/FAIL lines at the end) and the plot `interp-plot.png` (world position of the main mover against time, one dot per drawn frame, option off above and on below):

- **arithmetic**: the check line says 0 nodes differ (PASS), else the formulas are wrong and the rest is not to be trusted;
- **smooth**: the element that moves most in the cell (picked by the path of its world position): different positions per step of the movie, median: 1 with the option off (a staircase: 4 identical frames at 120 FPS), 3 or more with it on; for the pause list and the HUD;
- **cap 60 / cap 30**: with the option on the nodes are glided at 60 FPS (applied > 0) and left alone at 30 (applied = 0, the 0.6 rule);
- **duration**: the opening animation's gliding time and the number of `PAUSEMOVIE` advances are the same off and on within 60 ms and 2 advances (the steps are the movie's, so the animation is not faster or slower);
- **rest**: the pictures at rest (`rest-off.png`, `rest-on.png`, 0.5 scale) are pixel-identical: nothing moves, so nothing differs;
- the **census** per screen (how many nodes moved, changed colour, were new or another character, snapped, the largest move) and the SWF frame rates: the figures that say how much of the UI glides. The burst pictures are for looking at only: a picture takes tens of milliseconds, so they are not aligned to steps.

Not in the chain: frame time or CPU cost of the hook (a measured run would need the option on without the log; the log makes the hook many times dearer), Vulkan, frame generation, 3x (the same code, nothing resolution dependent), a real controller, the pictures of the other screens (pop-ups, prompts, transitions, title). Those come after the first look.

## Round 2 (2026-10-29, agent uiinterp2)

### Why round 1's report had missing cells (found in its data, nothing re-run)
The game rotates its log at 5 MB (`game.4.log` oldest to `game.1.log`, then `game.log`). Round 1's per-frame log was 21 MB, and the report read `game.log` only (the last 1.6 MB): the pad had run every cell, the report never saw the cap-120 and opening cells. `ui_interp.read_log()` reads the files in order now, and the runs pass `--log_max_file_size_mb=100`. Re-reading round 1's folder gave 10 cells and 70,807 rows: at a 120 FPS cap the HUD ticker (`HUDGAUGE` depth 3) and the pause rows are drawn at 4 different positions per 33 ms step with the option on, at 1 with it off; 0 differences in 8,113,592 node draws; applied in 3446 of 3446 gliding draws at cap 60, 0 of 1790 at cap 30. The rest pictures differed in a 21 x 19 px box on the selected tab's icon: its dashed ring turns by itself (looked at in the crop; round 2 checks it with three pictures per variant and the difference of two pictures of the same variant).

### What round 1's data says about snapping (measured, option off)
Largest single-step moves per node: the HUD ticker's wrap, 373.4 px in one step (-615.8 to -242.4), which the old 400 px limit let glide across the screen; the pause menu's opening slides, up to 62.8 px per step; the pause rows 8 px; everything else under 6 px. So: `mcla_ui_interp_snap_px` is 160 now, and a node that turns round and jumps more than 40 px, more than 8 times as far as the step before, is shown at once (the ticker's wrap), with a log line each (`ui interp event: .. kind=snap why=turned|far|flip`). A character swap (another shape at the same depth) is shown as it is and logged (`kind=swap`, the first 3, the 10th and the 100th of each node).

### Other changes
- With the option on every movie steps 30 times a second (all SWF files seen declare 7680 = 30.0 frames a second in 8.8 fixed point), whatever `mcla_ui_menu_rate` and `mcla_ui_hud_rate` say: the HUD's default 60 runs the ticker twice as fast as the console, and interpolation is the replacement for it.
- The rows of the log carry the matrix and colour (`lm`, `sm`, `cx`), so rotating and scaling nodes (needles, fills) are counted as movers, not only translating ones.
- Pad command `burst <prefix> <n> [scale] [x0 y0 x1 y1]`: n pictures of consecutive swaps, cropped (fractions of the frame), written after the last; the swap numbers are logged, so skipped frames show.

### The round-2 chain (queued by the main session; results in the agent's notes)
`launcher/dev/ui_interp.py pad trace|cost|big` writes the three pads, `report` reads them. One exclusive hold (about 16 to 22 minutes, 33 at most): build, then three game runs. **trace** (1x, per-frame log): the title screen, the HUD (full throttle then braking; the default rates; a burst of the ticker), the pause menu's closing and opening (cap 120 four times, cap 60 twice), the highlight moving (taps and a hold), three pictures at rest per variant, a burst, at caps 120 and 60 with the option off and on and at cap 30 with it on, a probe for a pop-up. **cost** (1x, no log): the HUD and the pause menu at cap 120 and uncapped, option off, on, off, on, 10 s each: the hook's own time (`mcla_ui_interp_stats`), the process's CPU per frame (`--mcla_profile_thread_log`, rows of one second inside the cell) and the frame times (median, p99) from `frames.csv` marks. **big** (3x): bursts, then the same cost cells.

Results: not in this note yet (the chain had not run when it was written).

# The sun's glare and the occlusion queries, 2026-10-28

In every run of the fork's builds that logged it, early in the session the SDK switched its host occlusion queries off and fed the game a fake sample count of 1,000, with the log line
`D3D12: occlusion query end at <addr> (swap N) found the query of another buffer: host occlusion queries off (begins 1, ends 1: 0 with a host result, 0 cut, 0 without one)`
(the line is from the diagnostic of the unmerged branch `fix/rain-scaled`, whose `occlusion_query_log` and counters this note's patch carries on). It happens at 1x and 2x, on D3D12 and on Vulkan.
Nothing of this was run in a game session by the author of the patch: it is read from code and old logs, compiled in CI, and its measurements are a chain that has not been queued yet.

## What the game does with the count (read: recompiled code, `SP\raintrace\asm.py`)

- One user was found: the sun. `sub_822DD720` (called by `sub_823069C0`; render phase `0x02000000` according to the earlier investigation, which the SDK's log with `occlusion_query_log` will show) calls `sub_822DD088` with the global object at `0x827E1048`.
  The object holds: `+0` the last count (samples), `+4` the ratio as a float, `+12` which of two query objects is in use (0 or 1, flipped every frame), `+16` and `+20` the two query objects,
  `+24` the expected count (1,250 in every run so far), `+28`, `+36`, `+40`, `+44` floats (20, 700, 200, 45), `+32` / `+33` flags.
- Each frame it reads the query object it used two frames earlier (`sub_82179720` = GetData: true only when the query has finished), keeps `+0` if it has not, computes
  `ratio = clamp(samples / expected, 0, c)` (stored at `+4`; the constant `c` was not read, presumably 1.0), then issues the same query object again: Begin (`sub_82179710`), one draw (`sub_8217EE80`, the sun's quad), End (`sub_82179718`).
  Then `sub_822DD328` draws the glare sprite with the strength `45 * ratio` (`+44` times `+4`). The queries are made again when a device parameter (`+8`) changes (inferred from the code, not seen).
- `sub_82179720`, `sub_82179710` and `sub_82179718` each have exactly one caller (`sub_822DD088`): the sun is the only occlusion query of the game's code reachable by a direct call. (Calls through a vtable were not searched: the chain's `users` run lists every record a session queries.)
- With the fake count: `samples = 1000`, `ratio = 1000 / 1250 = 0.8` at every peek of every earlier run (`000003E8 3F4CCCCD`). A fully visible sun should reach 1.0 (if the host counts the quad's pixels as the console does: not checked), so the glare is 20 % weaker than the game means it to be when the sun is in full view, and it does not fade when the sun is hidden.

## The cause (read)

- The query record. `sub_824302F0(device, query, is_begin)` sets `RB_SAMPLE_COUNT_ADDR` to `slot + (is_begin << 5)` and issues `EVENT_WRITE_ZPD` (PM4 header `0xC0005B00`, initiator 21). A record is two 32-byte `xe_gpu_depth_sample_counts`: the END snapshot at `+0x00` and the BEGIN snapshot at `+0x20`.
  End writes `0xFFFFFEED` into the END snapshot's ZPass words first (`sub_8241DE08`: by CPU stores, or by two PM4 memory writes in the command stream just before the event), the game's GetData (`sub_8241E088`) says "not finished" while both ZPass words still hold it, and computes END minus BEGIN.
  With predicated tiling the register is set once per tile slot (bin masked, `SET_BIN_MASK`); the SDK ignores bin masks, so the last slot's record gets the events.
- The SDK matched the two events by the address in the register and required it to be the same (`active_occlusion_query_.sample_count_address != sample_count_addr`). It never is. The log of the run `r2d-d3d12-x2-on` in `SP\rain17fix\runs\rain` has, in swap 4742,
  `occlusion query begin at 02D58020 ... host query` and `occlusion query end at 02D58000 ... found the query of another buffer: host occlusion queries off`: the same 64-byte record, begin snapshot 0x20 above the end snapshot.
- So the first end of every run (while the game boots into the city) called `DisableHostOcclusionQueries()` for good; the base class's fake path answered the rest of the run. The rain17fix chain's counters (`begins 1, ends 1, 0 with a host result, 0 cut`) are exactly this, and that branch's theory (a cut submission) was not the cause (`cut` was 0).
- The second defect, which the first hid: with matching addresses the end would have ended the whole submission and waited for the GPU (D3D12 `CheckSubmissionFence(query_submission)`, which blocks; Vulkan `AwaitAllQueueOperationsCompletion`): a stall of the command thread in the middle of the frame, at every query end.

## xenia-canary (read: `canary_experimental` `command_processor.cc`, 2026-10-27)

Its `occlusion_query` has four modes, `fake`, `fast` (the default), `fast-alt` and `strict`. It does not pair events at all: every ZPD event is a snapshot of a running cumulative counter that the guest subtracts, host query segments (a host query cannot span a D3D12 command list or a Vulkan render pass) are summed into it, and `fast` writes a guess at once (the last real delta of that report address, or one sample) and corrects it when the count lands.
`strict` waits for the GPU when the guest spins on the `0xFFFFFEED` sentinel. This patch takes the guess idea; it pairs by order because Direct3D 9 on the console does not nest occlusion queries, and it measures only between a begin and its end.

## The fix (`patches/rexglue-occlusion-queries.patch`)

- The events are paired by order: a begin starts the host query (and zeroes the begin snapshot), the next end ends it and queues a copy of the count into a readback buffer. Nothing waits.
- At the end event the game is told the last real count of that record (the fake count before the first one), so a game that reads the query at once gets a sensible number; the real count is written when the submission that holds the query has completed (`PollOcclusionQueryResults`: after every chunk of the primary buffer, at every end event and while the command thread idles), the ZPass words last and in one store.
  A count that arrives after the game has begun the record again is dropped.
- How the game tolerates a late count: it reads each query object two frames after it issued it (two objects in turn), and keeps its old value while the query says "not finished". A count one or two frames late is the console's own pipeline; a later one costs one stale frame of the glare, not a stall.
  The command thread never waits for it.
- `occlusion_query_keep_submission_open` (default on): a chunk end of the primary buffer inside a query no longer ends the submission (a host query cannot span submissions). A query cut by anything else gets its record's last real count; the counters show how often (`cut`).
- Vulkan: begun and ended outside render passes (the old safety net ended the query inside one), `VK_QUERY_CONTROL_PRECISE_BIT` when the device has `occlusionQueryPrecise`, the copy with `VK_QUERY_RESULT_WAIT_BIT` (waits on the GPU, not the CPU), and a transfer-to-host barrier.
- Off switches: `--occlusion_query_async=false` (the fake count, at once), `--occlusion_query_keep_submission_open=false`; `--occlusion_query_log=N` (development) logs the first N events and results, the first use of every record with the game's render phase, and the counters with every 120th result.

## Not known (nothing of the fix was run)

- That the host counts what the console counts. Two things could add samples inside the query: draws of the render target cache (ownership transfers, clears) when the sun's draw changes the render targets, and 4x MSAA / 2x resolution scale. The result lines log the samples on the host and the samples told to the game (the count divided by the scale squared) and the number of guest draws in the query (the sun's quad should be 1). A ratio over 1 at the first peeks, or a count far from 1,250 with the sun in full view, means this.
- That the sun's position in the view can be reached by the pad: the chain turns the camera round the car with the right stick at three times of day (16.5, 17.5, 18.5 h, the world's time poked as the dither pad of issue #8 does); if no step has a ratio over 0 the sun was never in view.
- The cost (the chain measures it: PresentMon, A B B A), the Vulkan path on any GPU but the RTX 5080, Linux, the PS5.
- The other occlusion mechanism: `PM4_VIZ_QUERY` (the console's visibility tests: draws predicated on a viz query, and "survey" draws with `kill_pix_post_hi_z`). The SDK fakes it as always visible (`ExecutePacketType3_VIZ_QUERY` sets the status bit at the end and logs `Begin viz query ID` / `End viz query ID`) and drops the survey draws; the predication is not implemented.
  `frame-structure-2026-10-16.md` lists "24 point draws with no target (occlusion queries for the glows)" in phase `0x20000000`, which may be this path (not identified). None of the earlier logs in `SP\rain17fix\runs` has a viz query line (a day city and a rainy night race at 2x), so the game may not use it there. The chain's SUMMARY lists the viz lines of every run.
- Other users: the `users` run (the whole session: title, zoom-out, free roam, the arcade race Sunset and Vine, the pause menu) lists every record with its render phase.

## The chain (SP\occl; `chain.ps1`, one exclusive hold of at most 45 minutes, results by `summarize.py`)

`SP\occl\scripts\occl_chain.py`'s header has the step table. The sun runs (D3D12 and Vulkan, 1x and 2x, fix off and on): free roam at the spawn, the car standing, fixed 1/60 s a frame; for each time of day the camera is turned round the car (right stick) in 60 steps of 8 frames;
every step reads the sun object (samples and ratio) and every second step takes a picture, so the same step is the same view with the fix off (ratio 0.8) and on. The cost runs: real time, uncapped, PresentMon, `occlusion_query_async` false, true, true, false (10 s each). `SUMMARY.txt` has the ratio of every step as a digit string, the counters, the latency in swaps, the host counts, the sheets
`sun-<api>-x<scale>.jpg` (rows: the brightest sun, just before and after it is lost, a partly covered one; columns: fix off, fix on, the difference x8), the cost tables and the records of the `users` run.


## Second round (the same day): what the first chain showed, and what changed

Every claim says whether it was measured (a log or a table of the first chain, `SP\occl\out\SUMMARY.txt`), read (the recompiled code or the SDK) or inferred.

### What the first chain showed (measured: RTX 5080, 15 runs, 11:23 to 11:38)

- **The pairing works.** No host query was turned off: D3D12 2x `begins` 4,460, `ends` 4,054, `host` 4,054, `delivered` 4,027, `superseded` 27, `dropped` 0, `cut` 0, `restarted` 406; Vulkan 2x 5,797 / 5,205 / 5,205 / 5,141 / 64 / 0 / 0 / 592; the same shape at 1x.
- **Every host count was 0.** D3D12 2x 4,027 of 4,027 results, Vulkan 2x 5,141 of 5,141, D3D12 1x 3,701 of 3,701, each query with 4 draws. So the sun object's ratio was 0 at all 180 steps of every sweep: the game was told a sun that is always hidden (the glare off), worse than the fake 0.8.
  **The exception:** Vulkan 1x (`s1v-vulkan-x1-on`): 1,089 of 5,099 results were above 0 (5,984 samples, constant, for 1,021 results before the pad's `title` mark and 68 of the 71 results of the title phase; then 5,994, 6,004, ... 6,471, 5,940 over 12 results while the camera started to move; then 0 for the map zoom-out, free roam and the three sweeps).
  So on Vulkan 1x the host path counted the sun's draws at the title scene. D3D12 1x, D3D12 2x and Vulkan 2x counted nothing in the same scene (D3D12 1x: 311 + 44 results before and in the title phase, all 0; Vulkan 2x: 742 + 427, all 0).
- **Half the ends were taken for begins.** The log has, in a period of four queries, two clean pairs (`begin 02D58020`, `end 02D58000`; `begin 02D58060`, `end 02D58040`) and two pairs where the end shows up as `begin at 02D58000 ... inside the open query begun at 02D58020` (`restarted`). The first version told an end from a begin by the sentinel `0xFFFFFEED` in the event's snapshot, and in those queries it was gone.
- **Latency.** 5 to 7 swaps from an end to its count at D3D12 and 3 to 5 at Vulkan, at about 240 FPS uncapped (the swap counter is the command processor's: a swap a frame in the city).
- **Cost.** A B B A at 2x D3D12: 178.95 fps (off) against 174.20 (on), equal arms differing by 5.9 and 6.0 fps; the second run (B A A B) gave the opposite sign (+2.69 %): within the noise; Vulkan 2x -1.35 % (equal arms 4.5 / 4.9); 1x +1.53 % and +0.10 %. No cost is measurable, which also says little while the counts are 0.

### Why the ends were taken for begins (read, and the log's pattern)

`sub_8241DE08` (the game's `Issue`) stores the sentinel in the end snapshot of the query's last slot in one of two ways: with the CPU, at once (`stw -275` to ZPass A and B), when the query has never been issued (`[query+24]` is 0) or its flag byte `[query+20]` has bit 7 set; or, when it has been issued and not consumed, with two PM4 memory writes
(`0xC0013D00`) followed by two `WAIT_REG_MEM` (`0xC0043C00`) in the command stream, so that the GPU's earlier write of the old count cannot land after it. `sub_8241E088` (`GetData`) sets bit 7 of `[query+20]` when it has read a snapshot that is not the sentinel.
In the first version, `GetData` finds the fence passed right after the end event and reads the guess (the record's last real count), sets the flag, the game's next `Issue(END)` for that query stores the sentinel with the CPU, and the real count of the earlier use, written late by `PollOcclusionQueryResults` into the same
end snapshot, lands on top of it before the command processor has run the new end event: the event finds no sentinel and the first version takes it for a begin (and zeroes the snapshot: the game then reads 0). The PM4 path is in order and safe. That makes the period of four queries seen in the log (flag set, flag not set, for each of the two query objects).
This is read from the code and fits the log; nothing was run to confirm it.

### What the second version does about it

An event 0x20 below the open begin's address is that query's end (the game's two snapshots are always 0x20 apart; `odd pairs` was 0), whatever the snapshot holds. The game is told, at every end event, the newest real count the host has delivered (before the first: the fake count); the count of the query itself is written to its record by one compare-and-swap
of the ZPass pair, only if the record still holds what the end event wrote there. So no write to the guest's memory races with the game's CPU store of the sentinel any more, and the count the game reads is as new as the host's queue allows (about the depth of the host's GPU queue: the latency above is that, not a delay of the SDK's; at a capped frame rate the queue is shorter, which the chain's `la*` runs measure).
`sentinel lost` counts the ends that were paired by address with no sentinel there; it should stay 0.

### Why the host counts were 0: not known; what each candidate predicts (the chain's diagnostic runs separate them)

1. **D3D12 does not rasterize the sun's draws** (inferred from the SDK's own comment in `pipeline_cache.cpp`: "rasterization (along with pixel counting) is disabled by disabling the pixel shader and depth / stencil", and its TODO about occlusion counting). It needs the sun's draws to have no depth test, no stencil and no colour write. The game's design (a query to see the sun hidden by buildings) argues for a depth test, so this is the less likely candidate;
   but the sun's pass (render phase `0x02000000`) is after the depth resolve (`frame-structure-2026-10-16.md`), and the glare sprite may sample the resolved depth itself. Prediction: the log's `occlusion draw` lines show `z=0` for the guest's own RB_DEPTHCONTROL with a colour mask 0, and `--occlusion_query_force_rasterization=false` (dd2) counts 0 where the default (dd1) counts above 0 at the title. The setting is the fix for this case on D3D12.
   On Vulkan the same draws would count (the render pass has the frame's attachments and counts without a depth test): consistent with 5,984 at Vulkan 1x, not with Vulkan 2x.
2. **A depth test against a depth buffer that is not the scene's at that point.** Prediction: `occlusion draw` lines with `z=1`, a func other than always, and `transfers depth` above 0 or a depth range unlike the scene's; the ROV and FSI runs (dd3, dv3) count the rasterized samples whatever the emulated depth test says: above 0 there, 0 in the default path, means the draws rasterize and the depth test removes everything.
3. **Something that depends on the resolution scale on Vulkan** (Vulkan 1x counts, 2x does not, in the same scene). Prediction: dv1 against dv2 differ in the draw lines (viewport, scissor, render targets, depth range) or not at all (then the count is wrong inside the host: `direct_host_resolve=false` (dv4) or the float24 conversion in the pixel shader (dv5) may change it).
4. **The draws are counted but not recorded.** The result lines now say `4 draws, N recorded`; `occlusion draw left out` lines say which draws never reached the host GPU and why (the registers).

### The second chain (`SP\occl2\chain.ps1`)

The diagnostic runs (D3D12 1x and 2x, Vulkan 1x and 2x; `render_target_path_d3d12=rov`, `render_target_path_vulkan=fsi`, `occlusion_query_force_rasterization=false`, `direct_host_resolve=false`, `depth_float24_convert_in_pixel_shader=true` on Vulkan, `async_shader_compilation=false`, `native_2x_msaa=false`; each the same short pad: boot, the title,
start, the zoom-out, free roam, one picture; about 40 s a run), then the sun sweeps with the fix off and on at 2x and 1x on both APIs, the latency runs (D3D12 capped at 60 and 30 FPS, Vulkan at 60), and the cost A B B A. `SUMMARY.txt` has, per run and phase, the host counts and the state of the draws, a table of the variables side by side, the sweeps' ratios, the sheets and the latency in ms.

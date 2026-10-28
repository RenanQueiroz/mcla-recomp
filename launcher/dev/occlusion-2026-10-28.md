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

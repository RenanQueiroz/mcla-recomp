# D3D12 upload hoisting, 2026-10-28

`patches/rexglue-d3d12-hoist-shared-memory-uploads.patch`, setting
`d3d12_hoist_shared_memory_uploads` (the SDK's default is off; **on by default in the Windows build** since v0.3.0, `ApplyPcDefaults` in `src/mcla_app.cpp`). The design and the
argument for "the same bytes reach the same draws" are in `patches/README.md`
(the section of the patch); this note is for the measurements.

## Why

On D3D12 every upload of guest memory into the shared memory buffer is copied in
place between two draws, with the buffer going from the reading state to the
copy destination state and back. From per-draw GPU timestamps (2026-10-28), the
draws that follow such a round trip took 2.6 ms longer in total than draws of the
same size after none on the Radeon iGPU at native resolution (471 draws after
1,019 round trips; 2.8 ms while driving), and about 1.9 ms of a 4.8 ms GPU frame
on the RTX 5080 at native resolution. These are upper bounds: the draw after an
upload also reads data that is not in the cache. The transitions alone were
measured with the shadow probe (2026-10-28): an empty shared memory round trip
(reading state to unordered access and back, no copy) before every 4th draw of
the shadow cascades cost 1.82 microseconds a pair on the Radeon iGPU at native
resolution (+0.805 ms for 444 pairs) and 1.70 microseconds on the RTX 5080
(+0.768 ms for 452 pairs); with about 1,000 round trips a frame that is about
1.7 to 1.9 ms in transitions on both GPUs, which is the part hoisting cuts to one pair per run of draws.
Vulkan hoists these uploads
already (`rexglue-vulkan-hoist-shared-memory-uploads.patch`, 27% shorter frame
on the RTX 5080 at native resolution).

## State

Written, compiled by CI, then run and measured on 2026-10-28 by a chain that is a
script outside the repository (its results are in the next section); what it did:

- exactness: the same frozen world drawn with the setting off, on, off, on, on
  the Radeon (Direct3D 12, native resolution) and the RTX 5080 (Direct3D 12, 3x),
  compared pixel for pixel; every picture of the same setting must also equal its
  twin (that is the noise floor: the frozen world must draw the same twice);
- timing: the setting off and on in the order A-B-B-A, frame time and GPU busy
  from PresentMon, free roam, on the Radeon (native resolution) and on the RTX 5080
  (native resolution and 3x); only adjacent runs are compared (the Radeon has hours
  when it runs 1.4 times slower);
- the Direct3D 12 debug layer (`--d3d12_debug=true`, Radeon, native resolution, a short
  pad) with the setting off and on, its messages counted: any kind of message that
  is new or more frequent with the setting on is a lead (the hoisted barriers and
  copies are the only new commands, and the layer checks the states they claim);
- `--d3d12_hoist_shared_memory_uploads_stats=true` logs the runs, the hoisted
  uploads and the uploads done in place, which says how much of the uploads the
  rule lets through.

## Results

All on the branch this patch was written on (the SDK of v0.2.0 plus this patch),
2026-10-28; the setting is `d3d12_hoist_shared_memory_uploads`, off against on.

**Exactness.** The first check (two pictures a side of a frozen free-roam world) was
found invalid the same day: with nothing changed at all it reported differences in 3
of 3 views on every run (worst case 732,205 pixels), because the world flickers in
roam views. The check that replaced it takes eight pictures a block, four on and
four off in an order balanced against drift (on off off on off on on off, and the
complementary order), with a frame cap that both settings hold, and compares the
count of pixels that separate on from off with the same count in blocks where the
setting is never changed ("sham" blocks, three of them; the median is the reference).
A pixel counts when every picture of one setting is below every picture of the other.
- Pause menu (the world stops; the whole city is drawn): the RTX 5080 at 3x, 0 of
  8,294,400 pixels in both scenes. The Radeon iGPU at native resolution, 98 and 76
  separated pixels, all in one cell of the 6x8 grid each: the pause icon's rotating
  dashed ring, an animation whose phase, at the 15 FPS cap, lined up with the order
  of the pictures by chance (looked at, crops in the notes of the check). Everything
  else in both pause scenes: 0.
- Free roam, frozen (three views each): Radeon iGPU, views 0 and 1 exact, view 2
  inconclusive (2 of 9 pixels); RTX 5080, view 0 exact, view 1 inconclusive (only one
  of the two blocks differed), view 2 different but marginal (4,026 separated pixels
  against a limit of 3,792; the sham blocks reached 2,672). No evidence of a real
  difference; not a proof for the pixels that flicker.
- The hoisting was exercised in every scene (the log's 5-second lines while frozen: for
  example 327,314 runs and 686,454 hoisted uploads in the RTX 5080 pause scene).
- The Direct3D 12 debug layer (`--d3d12_debug=true`, Radeon iGPU, native resolution, a
  short pad), off against on: 5,788 and 5,597 messages, nearly all #0 UNKNOWN, no
  message that is new or more frequent with the setting on.

**Timing** (A-B-B-A segments of 15 s in free roam, off on on off off on on off; the
numbers are the medians of the segments, drift-corrected against the neighbouring off
segments; negative is faster with the setting on):

| | Frame time | GPU busy | p99 frame time |
|---|---|---|---|
| RTX 5080, 3x, world running | -15.4% (7.38 to 6.34 ms) | -15.3% | -14.7% |
| RTX 5080, 3x, world frozen | -12.2% | -12.1% | -11.1% |
| RTX 5080, native, world running | +0.0% (4.14 ms; a game thread holds the frame) | -28.7% (3.19 to 2.34 ms) | -1.3% |
| Radeon iGPU, native, world running | -2.4% (36.4 to 35.5 ms) | -2.4% | -3.1% |

The Radeon has slow hours (its frame time varies by 8% between segments); the spread
of the drift-corrected change over the four segments was -6.2% to +0.9%. The run was
short (one run of eight segments per GPU and resolution).

**Not checked.** Anything but these two GPUs, Direct3D 12 at 2x, a race, and the
interplay with the batched rain copies (`resolve_unscaled_copy_batch`, merged after
these measurements; those copies write the shared memory from a compute shader at the
start of the next draw, and a resolve ends a run, so the order is kept by the code as
read, not by a run). The release validation of v0.3.0 runs the smoke batch with the
setting on.

## What to do if the pictures differ

Turn the setting off (`--d3d12_hoist_shared_memory_uploads=false`), and report
which scene differed; the likely causes are a GPU read of the shared memory that
the draws' page marks do not cover (see "Assumed, not shown" in the patch's
section) or a command that uses the buffer without ending the run
(`D3D12SharedMemory::EndHoistRun`).

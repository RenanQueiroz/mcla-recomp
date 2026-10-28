# D3D12 upload hoisting, 2026-10-28

`patches/rexglue-d3d12-hoist-shared-memory-uploads.patch`, setting
`d3d12_hoist_shared_memory_uploads` (default off for now). The design and the
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
upload also reads data that is not in the cache. Vulkan hoists these uploads
already (`rexglue-vulkan-hoist-shared-memory-uploads.patch`, 27% shorter frame
on the RTX 5080 at native resolution).

## State

Written, compiled by CI. **Not run, not measured.** The chain that builds it and
measures it is a script outside the repository (the notes of the agent that wrote
it say where); what it does:

- exactness: the same frozen world drawn with the setting off, on, off, on, on
  the Radeon (Direct3D 12, native resolution) and the RTX 5080 (Direct3D 12, 3x),
  compared pixel for pixel; every picture of the same setting must also equal its
  twin (that is the noise floor: the frozen world must draw the same twice);
- timing: the setting off and on in the order A-B-B-A, frame time and GPU busy
  from PresentMon, free roam, on the Radeon (native resolution) and on the RTX 5080
  (native resolution and 3x); only adjacent runs are compared (the Radeon has hours
  when it runs 1.4 times slower);
- `--d3d12_hoist_shared_memory_uploads_stats=true` logs the runs, the hoisted
  uploads and the uploads done in place, which says how much of the uploads the
  rule lets through.

## Results

Not measured yet.

## What to do if the pictures differ

Leave the setting off (its default), do not turn it on in the launcher, and report
which scene differed; the likely causes are a GPU read of the shared memory that
the draws' page marks do not cover (see "Assumed, not shown" in the patch's
section) or a command that uses the buffer without ending the run
(`D3D12SharedMemory::EndHoistRun`).

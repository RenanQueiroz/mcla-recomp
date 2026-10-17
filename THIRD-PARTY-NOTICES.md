# Third-party notices

This fork is GPL-3.0-or-later (see [`LICENSE`](LICENSE)). Some of its changes are
ported from projects under the MIT licence, whose notice has to travel with
the code. They are listed here with that notice.

## ForzaHorizon1-ReImagined

<https://github.com/Dyotson/ForzaHorizon1-ReImagined>

Ported from it:

- `patches/rexglue-vulkan-hoist-shared-memory-uploads.patch`: its patch 0036,
  ported to ReXGlue v0.10.0 and extended (see `patches/README.md`). The notice
  is also in the code it adds (`src/graphics/vulkan/command_processor.cpp`,
  above `CanHoistSharedMemoryUpload`).
- `patches/rexglue-threaded-submission-vulkan.patch`: its patch 0038, ported to
  ReXGlue v0.10.0. The notice is also in the code it adds (above
  `StartSubmissionThread`).
- `patches/rexglue-threaded-submission-d3d12.patch`: only the design of patch
  0038; the code is written for Direct3D 12.
- `src/achievement_toast.h`: the achievement toast wrapper, which
  ForzaHorizon1-ReImagined has from RecompiledHaloReach (below); the file
  carries both notices.
- `launcher/dev/dropped-patches/unchanged-fetch-constants-keep-bindings.patch`:
  the approach of its patch 0032, measured and not used.

```
MIT License

Copyright (c) 2026 Dyotson and Forza Horizon 1: ReImagined contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## RecompiledHaloReach

<https://github.com/Dyotson/RecompiledHaloReach>

- `src/achievement_toast.h`: the achievement toast wrapper, from its
  `reach-recomp/src/platform/achievement_toast.cpp` (commit `cb092d4`,
  2026-10-10), which ForzaHorizon1-ReImagined took the same day (`b5ccbe2`).

```
MIT License

Copyright (c) 2026 Dyotson and RecompiledHaloReach contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Xenia and xenia-canary

The shaders the fork adds to the SDK (direct texture loads, direct resolves
and the fixed 4x MSAA resolve) are built from Xenia's shader sources, and some
changes follow xenia-canary's approach. Both are under Xenia's BSD-3-Clause
licence, whose notice the ReXGlue SDK already carries in its `LICENSE`.

## AMD FidelityFX SDK (FSR 3.1)

<https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>, version 1.1.4, as
carried by the ReXGlue fork <https://github.com/rexglue/FidelityFX-SDK> at commit
`eee08db1688ac3d1275a70b728f4a8ba22914213` (AMD's v1.1.4 plus portability fixes).

Used for the FSR 3.1 anti-aliasing option (`patches/rexglue-temporal-aa.patch`
and `patches/rexglue-temporal-aa-shaders-vk.patch`):

- the ffx-api headers, unmodified, in the first patch as
  `thirdparty/fidelityfx-api/include` of the SDK, with this notice beside them
  (`thirdparty/fidelityfx-api/LICENSE.txt`);
- 40 compiled shaders of FSR 3's upscaler (SPIR-V, in the headers FidelityFX's
  shader compiler generates, made from that commit's source by
  `launcher/dev/fsr/fsr_vk_shaders.py`), in the second patch as
  `thirdparty/fidelityfx-shaders-vk` of the SDK, with this notice beside them
  (`LICENSE.txt` there): the only FidelityFX binaries in this repository, for
  builds where that compiler (a Windows program) can't run;
- FidelityFX's runtime, built from that commit's source when the SDK is built
  (`cmake/rexglue_fsr.cmake` fetches it and checks its tree hashes):
  `amd_fidelityfx_dx12.dll` and `amd_fidelityfx_vk.dll` installed next to
  `mcla.exe` on Windows; `libamd_fidelityfx_vk.so` with the SDK's libraries on
  Linux; static libraries linked into the title on the PS5.

```
This file is part of the FidelityFX SDK.

Copyright (C) 2024 Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

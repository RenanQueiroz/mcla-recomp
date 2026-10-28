# Third-party notices

This fork is GPL-3.0-or-later (see [`LICENSE`](LICENSE)). Some of its changes are
ported from, or follow, projects under the MIT and zlib licences, whose notice
has to travel with the code. They are listed here with that notice.

## ForzaHorizon1-ReImagined

<https://github.com/Dyotson/ForzaHorizon1-ReImagined>

Ported from it:

- `patches/rexglue-vulkan-hoist-shared-memory-uploads.patch`: its patch 0036,
  ported to ReXGlue v0.10.0 and extended (see `patches/README.md`). The notice
  is also in the code it adds (`src/graphics/vulkan/command_processor.cpp`,
  above `CanHoistSharedMemoryUpload`).
- `patches/rexglue-d3d12-hoist-shared-memory-uploads.patch`: the scheme of its patch 0036 (as ported to Vulkan above) applied to
  Direct3D 12, and the page bit code of that port. The notice is also in the code it adds
  (`src/graphics/d3d12/shared_memory.cpp`, above `BeginHoistedUpload`).
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
- `patches/rexglue-xma-decoder-fixes.patch`: the idea of its patch 0105
  (`0105-xma-unreadable-input-and-locked-contexts.patch`): `Work()` looks at the
  context again once it has the lock, and `Release()` clears a pending kick. The
  code is written for this fork; its input-buffer validation is not taken.

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

## Xenia, xenia-canary and Xenia Edge

The shaders the fork adds to the SDK (direct texture loads, direct resolves
and the fixed 4x MSAA resolve) are built from Xenia's shader sources, and some
changes follow xenia-canary's approach. Both are under Xenia's BSD-3-Clause
licence, whose notice the ReXGlue SDK already carries in its `LICENSE`.

`patches/rexglue-xma-decoder-fixes.patch` takes code and design from the XMA
decoder of xenia-canary (<https://github.com/xenia-canary/xenia-canary>) and of
Xenia Edge (<https://github.com/has207/xenia-edge>), both BSD-3-Clause with the
same `LICENSE` file ("Copyright (c) 2015, Ben Vanik. All rights reserved."; the
source files read "Copyright 2024 Xenia Canary. All rights reserved. Released
under the BSD license"): from `src/xenia/apu/xma_context_new.cc`, the commits
`6bef1fbed`, `7015473a7`, `aee0871dd` and `ade7e610b` of xenia-canary and
`5dd1cdbbf` of Xenia Edge (all by Herman S.), and the design of Edge's
`1fae8f43a` (decoding on the kicking thread). The code lands in the SDK's own
`src/audio/xma_context.cpp`, which already carries the same copyright and
permission notice, with the authors' comments kept and the commits named where
the code is. The decoder it changes was itself taken into xenia-canary from the
ReXGlue SDK in May 2026 (xenia-canary `b575c6841`).

```
Copyright (c) 2015, Ben Vanik.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the project nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## AMD FidelityFX SDK (FSR 3.1)

<https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>, version 1.1.4, as
carried by the ReXGlue fork <https://github.com/rexglue/FidelityFX-SDK> at commit
`eee08db1688ac3d1275a70b728f4a8ba22914213` (AMD's v1.1.4 plus portability fixes).

Used for the FSR 3.1 anti-aliasing option (`patches/rexglue-temporal-aa.patch`
and `patches/rexglue-temporal-aa-shaders-vk.patch`) and for FSR 3.1 frame
generation (`patches/rexglue-frame-generation.patch`, in development):

- the ffx-api headers, unmodified, in the first patch as
  `thirdparty/fidelityfx-api/include` of the SDK, with this notice beside them
  (`thirdparty/fidelityfx-api/LICENSE.txt`);
- compiled shaders of FSR 3's upscaler and, since frame generation, of frame
  interpolation and optical flow (SPIR-V, in the headers FidelityFX's
  shader compiler generates, made from that commit's source by
  `launcher/dev/fsr/fsr_vk_shaders.py`), in the second patch as
  `thirdparty/fidelityfx-shaders-vk` of the SDK, with this notice beside them
  (`LICENSE.txt` there): the only FidelityFX binaries in this repository, for
  builds where that compiler (a Windows program) can't run;
- FidelityFX's runtime, built from that commit's source when the SDK is built
  (`cmake/rexglue_fsr.cmake` fetches it and checks its tree hashes):
  `amd_fidelityfx_dx12.dll` and `amd_fidelityfx_vk.dll` installed next to
  `mcla.exe` on Windows; `libamd_fidelityfx_vk.so` with the SDK's libraries on
  Linux; static libraries linked into the title on the PS5. On Windows the
  runtimes also carry frame generation (its provider, frame interpolation and
  optical flow), and the Direct3D 12 one AMD's frame interpolation swap chain
  (which includes the Anti-Lag 2 header); `ffx_framegeneration.h` is among the
  ffx-api headers.

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

## NVIDIA NVAPI (Reflex)

<https://github.com/NVIDIA/nvapi>, release of 2026-09 (commit `70d337d`).

Used for NVIDIA Reflex on Direct3D 12 (`patches/rexglue-low-latency.patch`):
`src/ui/d3d12/d3d12_low_latency.cpp` of the SDK has the structures and enums of
NVAPI's `nvapi.h` that Reflex uses, with the same layouts, and the interface ids
of `nvapi_interface.h`. NVAPI itself is not linked: `nvapi64.dll` comes with the
NVIDIA driver and the functions are looked up with `nvapi_QueryInterface`.

```
SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: MIT

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

## AMD Anti-Lag 2

<https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK>, the DirectX 12 header
as carried by the FidelityFX SDK (`sdk/libs/antilag2/ffx_antilag2_dx12.h`).

Used for AMD Anti-Lag 2 on Direct3D 12 (`patches/rexglue-low-latency.patch`): the
header is `include/rex/ui/d3d12/amd_antilag2_dx12.h` of the SDK, with its notice in
the file and two small changes for llvm-mingw, marked MCLA there (a `reinterpret_cast`
of `GetProcAddress`'s result where AMD's has an MSVC-only `static_cast` from `void*`,
and the interface's `__CRT_UUID_DECL` for mingw-w64's `__uuidof`); it is otherwise
byte for byte AMD's. It loads `amdxc64.dll` from the AMD driver; nothing of AMD's is
shipped. The game's use of it has not been run on an AMD GPU.

```
This file is part of the Anti-Lag 2.0 SDK.

Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
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

## ReXGlue SDK (pull requests), and Xenia Canary (BSD 3-Clause)

<https://github.com/rexglue/rexglue-sdk>, <https://github.com/xenia-canary/xenia-canary>

Taken from open pull requests to the ReXGlue SDK, which are not merged there
(each is credited with its number, title and author in `patches/README.md`):

- `patches/rexglue-upstream-storage-flush.patch`: #529 (wolfiesch).
- `patches/rexglue-upstream-host-fp-state.patch`: #382 (Azkali) and #459 (WillianPepal).
- `patches/rexglue-upstream-linux-wine.patch`: #517 and #514 (MoonFuji) and #522 (Meesz).
- `patches/rexglue-upstream-small-fixes.patch`: #503 (JTM-rootstorm) and #490
  (synthic), the latter a port of Xenia Canary's commit `32889f5`.

They are under the SDK's own licence, BSD 3-Clause, whose text follows; the SDK
checkout the build makes carries it as `LICENSE`. The port of `32889f5` is also
under Xenia Canary's licence, the second text.

```
Copyright (c) 2026, Tom Clay <tomc@tctechstuff.com>

Portions of this software are derived from the Xenia project:
Copyright (c) 2022, Ben Vanik and Xenia project contributors
https://xenia.jp

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

```
Copyright (c) 2015, Ben Vanik.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the project nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
DEALINGS IN THE SOFTWARE.
```

## Trademarks

NVIDIA, NVIDIA Reflex, GeForce, G-SYNC, AMD, Radeon, AMD Anti-Lag, AMD FreeSync and AMD
FidelityFX are trademarks of their owners (NVIDIA Corporation, Advanced Micro Devices,
Inc.); Windows and Direct3D are Microsoft's, Vulkan is the Khronos Group's, PresentMon
is Intel's, and the other product names in this repository's documents belong to their
owners. This fork uses the names only, as plain text, to say which GPU feature or tool
is meant (the pause menu's and the launcher's low latency setting, the frame generation
and FSR settings, the measurements): no logos, and no endorsement by any of them is
implied.

## puff (zlib)

<https://github.com/madler/zlib/tree/develop/contrib/puff>

`src/shader_prepare.cpp`'s raw deflate decoder (`Inflater`, for the game's
archive when shaders are prepared) follows the design of Mark Adler's puff.c:
canonical Huffman codes decoded from the count of codes of each length. It is
rewritten for this file, not copied, and marked as such there.

```
Copyright (C) 2002-2013 Mark Adler, all rights reserved

This software is provided 'as-is', without any express or implied
warranty.  In no event will the author be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

Mark Adler    madler@alumni.caltech.edu
```

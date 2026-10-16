# Third-party notices

This fork is GPL-3.0-or-later (see [`LICENSE`](LICENSE)). Some of its changes are
ported from projects under the MIT licence, whose notice has to travel with
the code. They are listed here with that notice.

## ForzaHorizon1-ReImagined

<https://github.com/Dyotson/ForzaHorizon1-ReImagined>

Ported from it:

- `patches/rexglue-vulkan-hoist-shared-memory-uploads.patch`: its patch 0036,
  ported to ReXGlue v0.10.0 and extended (see `patches/README.md`).
- `patches/rexglue-threaded-submission-vulkan.patch`: its patch 0038, ported to
  ReXGlue v0.10.0.
- `patches/rexglue-threaded-submission-d3d12.patch`: only the design of patch
  0038; the code is written for Direct3D 12.
- `src/achievement_toast.h`: the achievement toast wrapper (it carries this
  notice too; ForzaHorizon1-ReImagined has the approach from
  RecompiledHaloReach).
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

## Xenia and xenia-canary

The shaders the fork adds to the SDK (direct texture loads, direct resolves
and the fixed 4x MSAA resolve) are built from Xenia's shader sources, and some
changes follow xenia-canary's approach. Both are under Xenia's BSD-3-Clause
licence, whose notice the ReXGlue SDK already carries in its `LICENSE`.

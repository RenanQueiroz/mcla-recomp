r"""Stand-ins for the headers `rexglue codegen` writes, so that the host sources (src/*.cpp)
can be compiled without a game: python host_stub.py <src folder> <output folder>

The real generated\default\mcla_init.h, mcla_pch.h and mcla_funcs*.h come from the
player's default.xex (every guest function, and the image's size), and are never in the
repository, in CI or in a release. What the host sources take from them is little: the SDK's
PPC headers, the externs of the image info, and the declarations of the guest functions they
call or hook (sub_XXXXXXXX, found here by reading the sources). This writes exactly that,
with the numbers of the image left at zero: for compile checks only (-fsyntax-only), never
to be linked or run.
"""
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2]) / "generated" / "default"
out.mkdir(parents=True, exist_ok=True)

names = set()
for path in sorted(list(src.glob("*.cpp")) + list(src.glob("*.h"))):
    text = path.read_text(encoding="utf-8", errors="replace")
    for m in re.finditer(r"\b(?:__imp__)?(sub_[0-9A-F]{8})\b", text):
        names.add(m.group(1))
    # kernel imports: DECLARE_REX_FUNC(NtClose) declares __imp__NtClose too
    for m in re.finditer(r"\b__imp__([A-Za-z_][A-Za-z0-9_]*)\b", text):
        if not m.group(1).startswith("sub_"):
            names.add(m.group(1))

(out / "mcla_pch.h").write_text(r'''// STAND-IN (scripts/ci/host_stub.py): not generated from a game; compile checks only.
#pragma once

#define REX_CONFIG_H_INCLUDED

#define REX_IMAGE_BASE 0ull
#define REX_IMAGE_SIZE 0ull
#define REX_CODE_BASE 0ull
#define REX_CODE_SIZE 0ull
#define REX_THUNK_RESERVE_SIZE 0ull
#define REXCRT_HEAP 0

#include <rex/ppc.h>
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/chrono/clock.h>
#include <rex/perf/counter.h>
#include <rex/thread/mutex.h>
#include <rex/system/mmio_handler.h>

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

extern const rex::PPCImageInfo PPCImageConfig;
extern PPCFuncMapping PPCFuncMappings[];

#ifndef DECLARE_REX_FUNC
#define DECLARE_REX_FUNC(name) \
  REX_EXTERN(name);            \
  REX_EXTERN(__imp__##name)
#endif
''', encoding="ascii")

lines = ["// STAND-IN (scripts/ci/host_stub.py): the guest functions the host sources name.",
         "#pragma once", "", '#include "mcla_pch.h"', ""]
lines += [f"DECLARE_REX_FUNC({n});" for n in sorted(names)]
(out / "mcla_funcs.h").write_text("\n".join(lines) + "\n", encoding="ascii")

(out / "mcla_init.h").write_text(
    '// STAND-IN (scripts/ci/host_stub.py)\n#pragma once\n\n#include "mcla_pch.h"\n#include "mcla_funcs.h"\n',
    encoding="ascii")
print(f"host stub: {len(names)} guest functions declared in {out}")

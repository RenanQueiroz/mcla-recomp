r"""Desk estimate (agent vram19): the scaled resolve buffer's heaps a frame log implies. Every resolve (R line: dst, len) writes the scaled resolve buffer at dst * s^2;
the SDK commits it in 16 MB heaps (D3D12 kScaledResolveHeapSizeLog2 = 24) and never releases them: heap index = (dst * s^2) >> 24 .. ((dst + len - 1) * s^2) >> 24.
    python -I resolveheaps.py LOG [LOG ...]"""
import re
import sys

def heaps(path, scale):
    used = set()
    total = 0
    dsts = set()
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.startswith("R "):
            continue
        f = dict(p.split("=", 1) for p in line.split()[1:] if "=" in p)
        dst, ln = int(f["dst"], 16), int(f["len"])
        if (dst, ln) in dsts:
            continue
        dsts.add((dst, ln))
        total += ln
        first = (dst * scale * scale) >> 24
        last = ((dst + ln - 1) * scale * scale) >> 24
        used.update(range(first, last + 1))
    return len(dsts), total, used

for path in sys.argv[1:]:
    print("==", path)
    for s in (1, 2, 3):
        n, total, used = heaps(path, s)
        print(f"  {s}x: {n} distinct resolves, {total / 2**20:.1f} MB of guest memory written ({total * s * s / 2**20:.0f} MB scaled), {len(used)} heaps of 16 MB = {len(used) * 16} MB committed"
              + ("" if s > 1 else " (not used at 1x)"))

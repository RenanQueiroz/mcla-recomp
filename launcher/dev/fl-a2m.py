r"""Reads a TMPFL frame log (launcher/dev/frame-log.sdk.patch) and lists the draws that use alpha to mask.

  python -I fl_a2m.py <label_0.txt> [--all] [--ps HASH]

Output: one summary line per (ps hash, msaa, offsets, depth state, render target) group with the draw numbers, then (with --all)
every a2m draw. Marker lines (M kind=... value=...) give the phase around the draw (the last phase marker before it).
RB_COLORCONTROL: bit 4 = alpha to mask enable, bits 24..31 = four 2-bit offsets (pixel (0,0), (0,1), (1,0), (1,1) of a quad);
alpha test = bit 3, function bits 0..2. RB_SURFACE_INFO bits 16..17 = MSAA (0 1x, 1 2x, 2 4x).
RB_DEPTHCONTROL: bit 1 z_enable... (printed raw as dc=).
"""
import re
import sys
from collections import defaultdict

path = sys.argv[1]
show_all = "--all" in sys.argv
want_ps = sys.argv[sys.argv.index("--ps") + 1].upper() if "--ps" in sys.argv else None
kv = re.compile(r"(\w+)=([^\s]+)")
groups = defaultdict(list)
phase = "?"
total = 0
a2m_total = 0
rows = []
for line in open(path, encoding="utf-8", errors="replace"):
    if line.startswith("M "):
        m = dict(kv.findall(line))
        phase = f"{m.get('kind', '?')}:{m.get('value', '?')}"
        continue
    if not line.startswith("D "):
        continue
    total += 1
    f = dict(kv.findall(line))
    cc = int(f["cc"], 16)
    si = int(f["si"], 16)
    msaa = (si >> 16) & 3
    if not (cc >> 4) & 1:
        continue
    a2m_total += 1
    offs = tuple((cc >> (24 + 2 * i)) & 3 for i in range(4))
    ps = f.get("ps", "?")
    if want_ps and ps != want_ps:
        continue
    key = (ps, msaa, offs, "at=%d" % ((cc >> 3) & 1), "dc=" + f.get("dc", "?"), "ci=" + f.get("ci", "?").split(",")[0], "cm=" + f.get("cm", "?"))
    groups[key].append(int(f["n"]))
    rows.append((int(f["n"]), phase, ps, f.get("cnt"), key))
print(f"{path}: {total} draws, {a2m_total} with alpha to mask")
for key, ns in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    ps, msaa, offs, at, dc, ci, cm = key
    print(f"  {len(ns):4d} draws  ps={ps} msaa={('1x', '2x', '4x', '?')[msaa]} offsets={offs} {at} {dc} {ci} {cm}  first n={ns[0]} last n={ns[-1]}")
if show_all:
    for n, ph, ps, cnt, key in rows:
        print(f"  n={n} phase={ph} ps={ps} cnt={cnt}")

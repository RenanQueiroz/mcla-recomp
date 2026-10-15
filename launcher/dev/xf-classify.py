"""Classifies logged ownership transfers (TMPXF) by what triggered them.
clear     = the draw binding the new target is a clear rectangle (screen-space
            rectangle list, depth/stencil always-pass replace, no blending)
back      = transfer whose source is a clear rectangle's target with the same
            base, pitch and format but another MSAA mode (the copy back)
other     = anything else
Volume in mega-samples (tiles x 80 x 16), at 1x scale.

usage: xf-classify.py LOG, LOG from edram-transfer-log.sdk.patch (D3D12): every
MCLA_XF_EVERY-th frame written to MCLA_XF_FILE. With
rexglue-clear-rectangle-draws.patch the clear rectangles no longer bind a
target, so turn it off (--clear_rectangle_draws=false) to see them."""
import collections, re, sys

CLEAR_PS = {"A4A965C189287B99", "0000000000000000"}
frames = []
cur = None
for line in open(sys.argv[1]):
    if line.startswith("FRAME"):
        cur = []
        frames.append(cur)
    elif line.startswith("DRAW") and cur is not None:
        f = dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)
        f["draw"] = int(line.split()[1])
        f["rts"] = []
        cur.append(f)
    elif line.startswith("  RT") and cur:
        m = re.match(r"  RT(\d) (\S+) len=(\d+)(.*)", line)
        xfs = re.findall(r"XF\[(\d+)-(\d+) from (\S+?)( \+hostdepth)?\]", m.group(4))
        cur[-1]["rts"].append((int(m.group(1)), m.group(2), int(m.group(3)), xfs))


def key_parts(k):  # C@1024/p16/2x/k_2_10_10_10_FLOAT
    m = re.match(r"([CD])@(\d+)/p(\d+)/(\d)x/(\S+)", k)
    return m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5)


def is_clear(f):
    return (f["prim"] == "8" and f["ps"] in CLEAR_PS and f["dc"] in ("00008777", "00000000")
            and f["bc"].startswith("00010001") and f["cc"] in ("00000000", "AA000004", "AA00000C"))


tot = collections.Counter()
detail = collections.Counter()
for fr in frames:
    clear_targets = set()
    for f in fr:
        c = is_clear(f)
        for slot, k, ln, xfs in f["rts"]:
            kd, base, pitch, msaa, fmt = key_parts(k)
            for s, e, src, hd in xfs:
                n = (int(e) - int(s)) * 80 * 16 / 1e6
                sd, sb, sp, sm, sf = key_parts(src)
                if c:
                    cat = "clear"
                elif (sd, sb, sp, sf) == (kd, base, pitch, fmt) and src in clear_targets:
                    cat = "back"
                else:
                    cat = "other"
                tot[cat] += n
                detail[(cat, f"{k} <- {src}")] += n
            if c:
                clear_targets.add(k)
nf = len(frames)
print(f"{nf} frames; transferred mega-samples per frame (1x scale):")
for k, v in tot.most_common():
    print(f"  {k:6s} {v / nf:6.2f}")
print("\ntop sources (per frame):")
for (cat, kk), v in detail.most_common(30):
    print(f"  {cat:6s} {v / nf:6.3f}  {kk}")

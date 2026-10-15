"""Analyses TMPGT frame timings (gt_*.csv: i,ticks,label; first line freq).
Categories: work between <x and >x marks is x (xfer = EDRAM ownership
transfers + resolve clears, resolve = IssueCopy, texload, upload); the rest
is 'draw' (guest draws and their state). On Vulkan, <rpend/>rpend and
<rpbegin/>rpbegin bracket render pass ends and begins. Per frame: total GPU time and the
category split; per pass (PASS markers = render target set changes): time,
draws (one <xfer per draw update) and category split.

usage: gt-analyze.py DIR [--passes N] [--frame K]"""
import collections, glob, os, re, statistics, sys

d = sys.argv[1]
show_passes = int(sys.argv[sys.argv.index("--passes") + 1]) if "--passes" in sys.argv else 0
frame_k = int(sys.argv[sys.argv.index("--frame") + 1]) if "--frame" in sys.argv else None

def load(path):
    lines = open(path).read().splitlines()
    freq = int(lines[0].split(",")[1])
    ev = []
    for l in lines[1:]:
        i, t, label = l.split(",", 2)
        ev.append((int(t) * 1e6 / freq, label))  # microseconds
    return ev

def analyse(ev):
    cats = collections.Counter()
    passes = []  # [label, time, draws, Counter]
    stack = []
    cur = ["(start)", 0.0, 0, collections.Counter()]
    passes.append(cur)
    for (t0, l0), (t1, l1) in zip(ev, ev[1:]):
        if l0.startswith("<"):
            stack.append(l0[1:])
        elif l0.startswith(">") and stack:
            stack.pop()
        elif l0.startswith("PASS"):
            m = re.match(r"PASS prev_draws=(\d+) \|(.*)", l0)
            if m: cur[2] = int(m.group(1))
            cur = [m.group(2) if m else l0[5:], 0.0, 0, collections.Counter()]
            passes.append(cur)
        cat = stack[-1] if stack else "draw"
        dt = max(0.0, t1 - t0)
        cats[cat] += dt
        cur[1] += dt; cur[3][cat] += dt
    total = ev[-1][0] - ev[0][0]
    return total, cats, passes

files = sorted(glob.glob(os.path.join(d, "gt_*.csv")), key=lambda p: int(re.search(r"gt_(\d+)", p).group(1)))
rows = []
for p in files:
    ev = load(p)
    if len(ev) < 50:
        continue
    total, cats, passes = analyse(ev)
    rows.append((p, total, cats, passes))
print(f"{len(rows)} frames timed in {d}")
allcats = ["draw", "xfer", "resolve", "texload", "upload"]
# Vulkan also marks render pass ends and begins (rpend, rpbegin).
allcats += sorted({c for r in rows for c in r[2]} - set(allcats))
print("frame  total_ms  " + "  ".join(f"{c:>8s}" for c in allcats) + "  passes  draws")
for p, total, cats, passes in rows:
    n = re.search(r"gt_(\d+)", p).group(1)
    print(f"{n:>5s}  {total/1000:8.2f}  " + "  ".join(f"{100*cats[c]/total:7.1f}%" for c in allcats) + f"  {len(passes):6d}  {sum(x[2] for x in passes):5d}")
med = {c: statistics.median(100 * r[2][c] / r[1] for r in rows) for c in allcats}
print("median " + f"{statistics.median(r[1] for r in rows)/1000:8.2f}  " + "  ".join(f"{med[c]:7.1f}%" for c in allcats))
if show_passes:
    # pass table for one frame (median-total frame by default)
    pick = rows[len(rows) // 2] if frame_k is None else [r for r in rows if re.search(rf"gt_{frame_k}\.csv$", r[0])][0]
    p, total, cats, passes = pick
    print(f"\npasses of {os.path.basename(p)} ({total/1000:.2f} ms), in order (merged consecutive same target), top by time marked *:")
    merged = []
    for lab, t, dr, c in passes:
        if merged and merged[-1][0] == lab:
            merged[-1][1] += t; merged[-1][2] += dr; merged[-1][3].update(c)
        else:
            merged.append([lab, t, dr, collections.Counter(c)])
    big = sorted(range(len(merged)), key=lambda i: -merged[i][1])[:show_passes]
    for i, (lab, t, dr, c) in enumerate(merged):
        if t < 50 and i not in big:
            continue
        split = " ".join(f"{k}={v/1000:.2f}" for k, v in c.most_common() if v > 20)
        print(f"{'*' if i in big else ' '} {i:3d} {t/1000:6.2f} ms {dr:5d} draws  {lab[:110]}  [{split}]")



"""Per-load breakdown of texture loads from TMPGT frames recorded with
texload-split-timestamps.sdk.patch: for each <texload..>texload, the time in
tlbar1 (barriers before the load dispatches), tldisp (dispatches), tlbar2
(barriers before the copy), tlcopy (buffer to image copy), rpend and the rest,
with the TLINFO size, and the copy and dispatch rates in GB/s.

usage: texload-split.py DIR [--min-us N] [--per-load]"""
import collections, glob, os, re, statistics, sys

d = sys.argv[1]
min_us = float(sys.argv[sys.argv.index("--min-us") + 1]) if "--min-us" in sys.argv else 0
per_load = "--per-load" in sys.argv
steps = ["tlbar1", "tldisp", "tlbar2", "tlcopy", "rpend", "texload"]
frames = []
loads_all = []
for p in sorted(glob.glob(os.path.join(d, "gt_*.csv")), key=lambda p: int(re.search(r"gt_(\d+)", p).group(1))):
    lines = open(p).read().splitlines()
    freq = int(lines[0].split(",")[1])
    ev = [(int(l.split(",", 2)[1]) * 1e6 / freq, l.split(",", 2)[2]) for l in lines[1:]]
    if len(ev) < 50:
        continue
    stack = []
    cur = None
    loads = []
    for (t0, l0), (t1, l1) in zip(ev, ev[1:]):
        if l0 == "<texload":
            cur = {"info": "", "bytes": 0, **{s: 0.0 for s in steps}}
        if l0.startswith("TLINFO") and cur is not None:
            cur["info"] = l0[7:]
            m = re.search(r"bytes=(\d+)", l0)
            cur["bytes"] = int(m.group(1)) if m else 0
        if l0.startswith("<"):
            stack.append(l0[1:])
        elif l0.startswith(">") and stack:
            stack.pop()
            if l0 == ">texload" and cur is not None and "texload" not in stack:
                loads.append(cur)
                cur = None
        if cur is not None and stack:
            top = stack[-1]
            cur[top if top in steps else "texload"] += max(0.0, t1 - t0)
    frames.append((p, loads))
    loads_all += loads

def total(l):
    return sum(l[s] for s in steps)

print(f"{len(frames)} frames in {d}")
print("frame  loads  total_ms " + " ".join(f"{s:>8s}" for s in steps))
for p, loads in frames:
    n = re.search(r"gt_(\d+)", p).group(1)
    print(f"{n:>5s}  {len(loads):5d}  {sum(map(total, loads))/1000:7.2f} " +
          " ".join(f"{sum(l[s] for l in loads)/1000:8.3f}" for s in steps))
# Rates for the big loads: bytes is the scratch buffer size (about the texture size).
big = [l for l in loads_all if total(l) >= min_us and l["bytes"]]
by_info = collections.defaultdict(list)
for l in big:
    by_info[l["info"]].append(l)
print("\nmedian per kind of load (" + str(len(big)) + f" loads of at least {min_us} us):")
print("   count      MB  total_us  disp_us  copy_us  bar1_us  bar2_us  disp_GB/s  copy_GB/s  info")
for info, ls in sorted(by_info.items(), key=lambda kv: -statistics.median(total(l) for l in kv[1])):
    med = lambda k: statistics.median(l[k] for l in ls)
    mb = ls[0]["bytes"] / 1e6
    disp, copy = med("tldisp"), med("tlcopy")
    print(f"  {len(ls):6d} {mb:7.2f} {statistics.median(map(total, ls)):9.1f} {disp:8.1f} {copy:8.1f} "
          f"{med('tlbar1'):8.1f} {med('tlbar2'):8.1f} {mb*1e3/disp if disp else 0:10.1f} "
          f"{mb*1e3/copy if copy else 0:10.1f}  {info}")
if per_load:
    for l in big:
        print(f"{total(l):8.1f} " + " ".join(f"{s}={l[s]:.1f}" for s in steps) + f"  {l['info']}")

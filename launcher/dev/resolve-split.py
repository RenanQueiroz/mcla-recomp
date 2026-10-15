"""Resolves and texture loads per frame, from GPU timestamp frames recorded with
gpu-timestamps(-vulkan).sdk.patch, texload-split-timestamps.sdk.patch and
resolve-split-timestamps.sdk.patch: per frame the resolves (the dump into the
EDRAM buffer, the resolve shader, or the direct resolve shader with
direct_host_resolve, the clear) and the texture loads; per kind
of resolve (size, formats, EDRAM range, destination) and of load (size,
format, address) the time a frame; with --detail N, frame gt_N.csv in order.

usage: resolve-split.py DIR [--detail N]"""
import collections, glob, os, re, statistics, sys

d = sys.argv[1]
detail = sys.argv[sys.argv.index("--detail") + 1] if "--detail" in sys.argv else None


def kv(s):
    return dict(x.split("=", 1) for x in s.split()[1:] if "=" in x)


frames = []
for p in sorted(glob.glob(os.path.join(d, "gt_*.csv")), key=lambda p: int(re.search(r"gt_(\d+)", p).group(1))):
    lines = open(p).read().splitlines()
    freq = int(lines[0].split(",")[1])
    ev = [(int(l.split(",", 2)[1]) * 1e6 / freq, l.split(",", 2)[2]) for l in lines[1:]]
    if len(ev) < 50:
        continue
    items = []  # dicts: kind resolve/load, info, times per scope
    stack = []
    cur = None
    pending_rs = None
    pass_name = ""
    for (t0, l0), (t1, l1) in zip(ev, ev[1:]):
        dt = max(0.0, t1 - t0)
        if l0.startswith("PASS"):
            pass_name = l0.split("|", 1)[1] if "|" in l0 else l0
        if l0 == "<resolve":
            cur = {"kind": "resolve", "info": {}, "t": collections.Counter(), "pass": pass_name, "start": t0}
            items.append(cur)
        elif l0 == "<texload" and (not stack or stack[-1] != "resolve"):
            cur = {"kind": "load", "info": {}, "t": collections.Counter(), "pass": pass_name, "start": t0}
            items.append(cur)
        if l0.startswith("RSINFO") and cur is not None:
            cur["info"] = kv(l0)
        if l0.startswith("TLINFO") and cur is not None:
            cur["info"] = kv(l0)
        if l0.startswith("<"):
            stack.append(l0[1:])
        elif l0.startswith(">") and stack:
            stack.pop()
        if cur is not None and stack and stack[0] in ("resolve", "texload"):
            cur["t"][stack[-1]] += dt
        elif not stack:
            cur = None
    total = ev[-1][0] - ev[0][0]
    frames.append((p, total, items))

print(f"{len(frames)} frames")
# Per-frame totals.
for p, total, items in frames:
    rs = [i for i in items if i["kind"] == "resolve"]
    ld = [i for i in items if i["kind"] == "load"]
    print(f"{os.path.basename(p):10s} frame {total/1000:6.2f} ms  resolves {len(rs):3d} "
          f"{sum(sum(i['t'].values()) for i in rs)/1000:5.2f} ms (dump {sum(i['t']['rdump'] for i in rs)/1000:4.2f} "
          f"copy {sum(i['t']['rcopy'] for i in rs)/1000:4.2f} direct {sum(i['t']['rdirect'] for i in rs)/1000:4.2f} "
          f"bar {sum(i['t']['rbar'] for i in rs)/1000:4.2f} "
          f"clear {sum(i['t']['rclear'] for i in rs)/1000:4.2f})  loads {len(ld):3d} "
          f"{sum(sum(i['t'].values()) for i in ld)/1000:5.2f} ms")


def rkey(i):
    f = i["info"]
    return (f"{'D' if f.get('depth')=='1' else 'C'} {f.get('w')}x{f.get('h')} fmt={f.get('fmt')} msaa={f.get('msaa')} "
            f"efmt={f.get('efmt')} ebase={f.get('ebase')} epitch={f.get('epitch')} dst={f.get('base')} "
            f"len={f.get('len')} clr={f.get('cdepth')}{f.get('ccolor')} smp={f.get('sample')}")


def lkey(i):
    f = i["info"]
    return (f"{f.get('w')}x{f.get('h')} fmt={f.get('fmt')} lv={f.get('levels')} sc={f.get('scaled')} "
            f"addr={int(f.get('addr', 0)):08X} gbase={f.get('gbase')} mb={int(f.get('bytes', 0))/1e6:.1f}")


for kind, keyf in (("resolve", rkey), ("load", lkey)):
    agg = collections.defaultdict(list)
    for p, total, items in frames:
        per = collections.defaultdict(lambda: [0, collections.Counter()])
        for i in items:
            if i["kind"] != kind:
                continue
            k = keyf(i)
            per[k][0] += 1
            per[k][1].update(i["t"])
        for k, (n, t) in per.items():
            agg[k].append((n, t))
    print(f"\n{kind}s by kind: frames seen, count/frame, median us/frame total and per step")
    rows = []
    for k, v in agg.items():
        tot = statistics.median(sum(t.values()) for n, t in v) if len(v) else 0
        rows.append((sum(sum(t.values()) for n, t in v) / len(frames), k, v))
    for avg, k, v in sorted(rows, key=lambda r: -r[0])[:40]:
        steps = collections.Counter()
        for n, t in v:
            steps.update(t)
        steps = {s: round(x / len(frames)) for s, x in steps.items() if x / len(frames) >= 5}
        print(f"  {avg:7.0f} us/frame  in {len(v):2d}/{len(frames)} frames  x{statistics.median(n for n, t in v):.0f}  {k}  {steps}")

if detail:
    for p, total, items in frames:
        if not p.endswith(f"gt_{detail}.csv"):
            continue
        print(f"\n--- {p}")
        for i in items:
            k = rkey(i) if i["kind"] == "resolve" else lkey(i)
            print(f"{i['start']/1000:8.3f} {i['kind']:7s} {sum(i['t'].values()):7.1f} us  {k}  "
                  f"{ {s: round(x) for s, x in i['t'].items()} }  [{i['pass'][:90]}]")

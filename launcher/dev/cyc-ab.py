"""In-run A/B of GPU command thread cost from TMPSUB_CSV (t_us,state,cycles,
draws,record_us; one row per swap). Windows = runs of rows with the same
state; the first SETTLE s after a switch, the first SKIP s of the run and
frames with < 1000 draws (loading, the attract movie) are left out. Each
state-1 window is compared with the mean of its neighbouring state-0
windows on GPU-thread cycles per draw (sum/sum), which cancels slow scene
drift; prints the mean difference with its standard error and halves."""
import csv, os, statistics, sys
SETTLE = float(os.environ.get("SETTLE_S", "1")) * 1e6
SKIP = float(os.environ.get("SKIP_S", "20")) * 1e6
for path in sys.argv[1:]:
    rows = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(open(path)) if None not in r.values() and "" not in r.values()]
    if not rows:
        print(path, "empty"); continue
    t0 = rows[0]["t_us"]
    wins, cur, start = [], None, None
    for r in rows:
        if cur is None or r["state"] != cur["s"]:
            cur = {"s": r["state"], "c": 0, "d": 0, "f": 0, "rec": 0, "us": 0}; wins.append(cur); start = r["t_us"]
        if r["t_us"] - start < SETTLE or r["t_us"] - t0 < SKIP or r["draws"] < 1000:
            continue
        cur["c"] += r["cycles"]; cur["d"] += r["draws"]; cur["f"] += 1; cur["rec"] += r["record_us"]
    wins = [w for w in wins if w["f"] >= 30]
    name = os.path.basename(path)
    print(f"== {name}: {len(wins)} windows")
    for s in (1, 0):
        ws = [w for w in wins if w["s"] == s]
        if not ws: continue
        c = sum(w["c"] for w in ws); d = sum(w["d"] for w in ws); f = sum(w["f"] for w in ws); rec = sum(w["rec"] for w in ws)
        print(f"   state {s}: {len(ws):2d} windows, {f:6d} frames | {c / d / 1e3:8.1f} kcycles/draw | {c / f / 1e6:6.2f} Mcycles/frame | {d / f:6.0f} draws/frame | recording {rec / f / 1000:.3f} ms/frame")
    diffs = []
    for i, w in enumerate(wins):
        if w["s"] != 1: continue
        nb = [wins[j] for j in (i - 1, i + 1) if 0 <= j < len(wins) and wins[j]["s"] == 0]
        if nb:
            off = statistics.fmean(x["c"] / x["d"] for x in nb)
            diffs.append((w["c"] / w["d"] - off) / off * 100)
    if len(diffs) > 2:
        h = len(diffs) // 2
        se = statistics.stdev(diffs) / len(diffs) ** 0.5
        print(f"   paired state1 vs state0, GPU-thread cycles per draw: {statistics.fmean(diffs):+.1f}% +/- {se:.1f}% (n={len(diffs)}); halves {statistics.fmean(diffs[:h]):+.1f}% / {statistics.fmean(diffs[h:]):+.1f}%")

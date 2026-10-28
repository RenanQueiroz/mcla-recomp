r"""Desk check (agent vram19): does a cap of CAP rows cut anything a frame log shows? For every render target key with a known extent (the viewport or window scissor of its draws), the rows
it needs against the host height the cap would give it. Keys whose draws carry no extent (a screen-space draw with the viewport off) are listed as unknown.
    python -I checkcap.py [CAP] LOG [LOG ...]"""
import glob
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import importlib
rc = importlib.import_module("vram-rtcensus")

args = sys.argv[1:]
cap = 1024
if args and args[0].isdigit():
    cap = int(args.pop(0))
logs = args or [r"C:\mcv\dither\fl\fl_dusk_0.txt", r"C:\mcv\dither\fl\fl_night_0.txt", r"C:\mcv\rain17\fl\p2d-d3d12-x2-gt\rain_0.txt", r"C:\mcv\rain17\fl\p1d-d3d12-x1-gt\rain_0.txt"]
worst = 0
for path in logs:
    keys, need = rc.census(path)
    unknown = ok = 0
    for k in keys:
        kind, base, fmt, pt, msaa = k
        n = need.get(k)
        if not n:
            unknown += 1
            continue
        y_log2 = 1 if msaa >= 1 else 0
        full_rows = min(2048 // pt, (8192 << y_log2) // 16) * (16 >> y_log2)
        capped = min(full_rows, (cap // (16 >> y_log2)) * (16 >> y_log2))
        worst = max(worst, n)
        if n > capped:
            print("CUT", path, k, "needs", n, "capped", capped)
        else:
            ok += 1
    print(f"{Path(path).name}: {len(keys)} keys; extent known and within the cap {ok}; extent unknown {unknown}")
print("largest known extent (rows):", worst, "; cap", cap)

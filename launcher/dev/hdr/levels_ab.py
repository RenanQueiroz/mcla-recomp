"""HDR levels from make_levels_ab_pad.py captures (debug 8: PQ codes as grey,
8 bits). Per scene, (peak, paper) and highlights, for the shoulder before the
review fix (old) and after it (new) in the same paused frame: the brightest
channel's max, 99.9th and 99th percentiles (nits) over the frame; right of
the pause menu, the HDR/SDR ratio of the brightest channel (median, 5th
percentile) for pixels whose 8-bit level is 0.8..1.0 of white (near the
game's white); and how many pixels differ between old and new and by how
much (median of new/old where they differ).
usage: levels_ab.py DIR"""
import sys, os
import numpy as np
from PIL import Image

d = sys.argv[1]


def pq_eotf(c):
    e = np.power(c, 1 / 78.84375)
    y = np.maximum(e - 0.8359375, 0) / (18.8515625 - 18.6875 * e)
    return 10000 * np.power(y, 1 / 0.1593017578125)


def nits(p):
    return pq_eotf(np.asarray(Image.open(p).convert("RGB")).astype(np.float64) / 255.0).max(axis=2)


for scene in ("day", "dusk", "night"):
    for peak, paper in ((1317, 280), (600, 240)):
        ref_path = os.path.join(d, f"{scene}_{peak}_{paper}_sdr_d8.png")
        if not os.path.exists(ref_path):
            continue
        s = nits(ref_path)
        x0 = int(s.shape[1] * 0.47)
        sr = s[:, x0:] / paper
        band = (sr >= 0.8) & (sr < 1.0)
        for hl in ("1", "2"):
            row = f"{scene:5s} {peak:4d}/{paper:3d} h{hl}"
            vals = {}
            for kind in ("old", "new"):
                p = os.path.join(d, f"{scene}_{peak}_{paper}_h{hl}" + ("_old" if kind == "old" else "") + "_d8.png")
                h = nits(p)
                vals[kind] = h
                r = (h[:, x0:] / paper)[band] / sr[band]
                row += (f" | {kind}: max {h.max():5.0f} p99.9 {np.percentile(h, 99.9):5.0f} p99 {np.percentile(h, 99):4.0f}"
                        f" near-white HDR/SDR {np.median(r):5.3f}/{np.percentile(r, 5):5.3f}")
            diff = vals["new"] != vals["old"]
            ratio = vals["new"][diff] / np.maximum(vals["old"][diff], 1e-6)
            row += f" | differ {diff.mean() * 100:5.2f}% px, new/old median {np.median(ratio) if diff.any() else 1:5.3f}"
            print(row)
        print(f"{'':16s} near-white band: {band.sum()} px")

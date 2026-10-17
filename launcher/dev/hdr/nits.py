"""HDR levels from hdr_debug_ui 8 captures (the output's HDR10 code values as
grey levels): per scene, nits of the brightest channel (BT.2020) and of
luminance.  usage: nits.py DIR name... [--paper P]"""
import sys, os, numpy as np
from PIL import Image
args = sys.argv[1:]
paper = 240.0
if "--paper" in args:
    i = args.index("--paper"); paper = float(args[i + 1]); del args[i:i + 2]
d = args[0]
def pq_eotf(c):
    e = np.power(c, 1 / 78.84375)
    y = np.maximum(e - 0.8359375, 0) / (18.8515625 - 18.6875 * e)
    return 10000 * np.power(y, 1 / 0.1593017578125)
for n in args[1:]:
    p = os.path.join(d, f"{n}_d8.png")
    a = np.asarray(Image.open(p).convert("RGB")).astype(np.float64) / 255.0
    nits = pq_eotf(a)
    lum = nits @ np.array([0.2627, 0.6780, 0.0593])
    mx = nits.max(axis=2)
    print(f"{n:12s} max channel: max {mx.max():7.0f}  p99.9 {np.percentile(mx, 99.9):6.0f}  p99 {np.percentile(mx, 99):6.0f}  median {np.median(mx):6.1f} | lum max {lum.max():6.0f} median {np.median(lum):5.1f} | >paper {np.mean(mx > paper * 1.02) * 100:5.2f}%  >2x {np.mean(mx > 2 * paper) * 100:5.2f}%")

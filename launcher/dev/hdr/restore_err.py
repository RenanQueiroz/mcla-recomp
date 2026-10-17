"""Restore error: shots with the UI layer off (the game's frame) against shots
with it on (hdr_debug_ui 2: the game's frame after the restore), over pixels
that are still across all shots of each kind.  usage: restore_err.py DIR"""
import glob, os, sys
import numpy as np
from PIL import Image
d = sys.argv[1]
for scene in ["roam", "pause", "gps"]:
    A = [np.asarray(Image.open(f).convert("RGB")).astype(int) for f in sorted(glob.glob(os.path.join(d, f"{scene}_off_*.png")))]
    B = [np.asarray(Image.open(f).convert("RGB")).astype(int) for f in sorted(glob.glob(os.path.join(d, f"{scene}_on_*.png")))]
    if not A or not B: continue
    still = np.ones(A[0].shape[:2], bool)
    for X in (A, B):
        for x in X[1:]:
            still &= (x == X[0]).all(axis=2)
    diff = np.abs(A[0] - B[0]).max(axis=2)[still]
    hist = np.bincount(np.minimum(diff, 9), minlength=10) / diff.size * 100
    print(f"{scene:6s} still {still.mean()*100:5.1f}%: exact {hist[0]:6.2f}%  1:{hist[1]:5.2f}% 2:{hist[2]:5.2f}% 3:{hist[3]:5.2f}% >3:{hist[4:].sum():5.2f}%  max {diff.max()}  mean {diff.mean():.3f}")

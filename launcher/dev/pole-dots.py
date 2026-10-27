r"""Counts the lattice dots on the lamp pole inside the shop window at the dusk spawn of free roam (the owner's img1 place), by pixel parity.

  python -P -E pole_dots.py <png> [<png> ...]      region x 150..167, y 185..254 of the 1280x720 picture (the pole is x 155..159)

A dot = a pixel of a dark pole column (column median luma under 40) brighter than that median by more than 8 levels. With alpha to mask the
holes are at x odd, y even (the quad's offset-2 pixel, the one whose threshold is 1.0): the line prints the counts per (x%2, y%2) and the total.
"""
import sys
import numpy as np
from PIL import Image

X0, X1, Y0, Y1 = 150, 168, 185, 255
for f in sys.argv[1:]:
    im = np.asarray(Image.open(f).convert("RGB"), dtype=float)
    if im.shape[0] != 720:
        print(f"{f}: {im.shape[1]}x{im.shape[0]} is not 1280x720, skipped")
        continue
    L = im.mean(2)
    counts = {(0, 0): 0, (0, 1): 0, (1, 0): 0, (1, 1): 0}
    for x in range(X0, X1):
        col = L[Y0:Y1, x]
        med = float(np.median(col))
        if med >= 40:
            continue
        for y in range(Y0, Y1):
            if L[y, x] > med + 8:
                counts[(x % 2, y % 2)] += 1
    total = sum(counts.values())
    print(f"{f}: dots {total}  (x%2,y%2): " + "  ".join(f"{k}={v}" for k, v in counts.items()) + f"   x odd y even share {100 * counts[(1, 0)] / total if total else 0:.0f}%")

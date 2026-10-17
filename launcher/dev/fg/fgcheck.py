"""Checks frame generation's dumps (frame_generation_dump on D3D12: fg_dump_d3d12_<frame>_r<refresh>_
{real,generated,real_only}_<w>x<h>.ppm, HDR10 codes, the guest output's size).

For every generated frame G(k) whose neighbours R(k-1) and R(k) were dumped too:
  still: of the pixels equal in R(k-1) and R(k) (the HUD, a still scene), the share where G(k) is
         within 2 codes of them (generation and its compose should leave them as they are);
  outside: the share of pixels where G(k) is more than 8 codes outside the range the neighbours span
         around it (--window pixels, for motion): what interpolation should not produce (garbage,
         wrong colours);
  mean: G(k)'s mean absolute difference from the neighbours' average, in codes.
Frames dumped without a generated image (generation skipped) are counted.

    python fgcheck.py <dump folder> [--crops <folder>]
"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np

NAME = re.compile(r"fg_dump_d3d12_(\d+)_r(\d+)_(real|generated|real_only)_(\d+)x(\d+)\.ppm$")


def read_ppm(path):
    data = path.read_bytes()
    # "P6 w h maxval\n"
    header_end = data.index(b"\n") + 1
    _, w, h, maxval = data[:header_end].split()
    w, h, maxval = int(w), int(h), int(maxval)
    if maxval > 255:
        pixels = np.frombuffer(data, dtype=">u2", offset=header_end)
    else:
        pixels = np.frombuffer(data, dtype=np.uint8, offset=header_end)
    return pixels.reshape(h, w, 3).astype(np.int32)


def downscale(img, n):
    if n <= 1:
        return img
    h, w = img.shape[0] // n * n, img.shape[1] // n * n
    return img[:h, :w].reshape(h // n, n, w // n, n, 3).mean(axis=(1, 3)).round().astype(np.int32)


def bilinear(img, shape):
    """img scaled to shape[:2] as the presenter's bilinear pass does: each output pixel's centre
    mapped into the source, the four nearest texels weighted, edges clamped."""
    h, w = shape[:2]
    sh, sw = img.shape[:2]
    y = np.maximum((np.arange(h) + 0.5) * sh / h - 0.5, 0.0)
    x = np.maximum((np.arange(w) + 0.5) * sw / w - 0.5, 0.0)
    y0 = np.minimum(np.floor(y).astype(np.int64), sh - 1)
    x0 = np.minimum(np.floor(x).astype(np.int64), sw - 1)
    y1 = np.minimum(y0 + 1, sh - 1)
    x1 = np.minimum(x0 + 1, sw - 1)
    fy = (y - np.floor(y))[:, None, None]
    fx = (x - np.floor(x))[None, :, None]
    f = img.astype(np.float64)
    top = f[y0][:, x0] * (1 - fx) + f[y0][:, x1] * fx
    bottom = f[y1][:, x0] * (1 - fx) + f[y1][:, x1] * fx
    return np.rint(top * (1 - fy) + bottom * fy).astype(np.int32)


def window(img, radius, op):
    """op (np.minimum or np.maximum) over the (2 radius + 1)^2 neighbourhood of each pixel."""
    padded = np.pad(img, ((radius, radius), (radius, radius), (0, 0)), mode="edge")
    h, w = img.shape[:2]
    out = img.copy()
    for dy in range(2 * radius + 1):
        for dx in range(2 * radius + 1):
            out = op(out, padded[dy:dy + h, dx:dx + w])
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("folder")
    parser.add_argument("--crops", help="write crops of the worst places here")
    parser.add_argument("--window", type=int, default=6,
                        help="how far (pixels) a generated pixel may have moved from the neighbours' "
                             "ones (the range is taken over that neighbourhood)")
    parser.add_argument("--downscale", type=int, default=1,
                        help="compare at 1/N size (box): the size presented, for frames generated "
                             "at the window's size")
    args = parser.parse_args()
    frames = {}
    for path in Path(args.folder).glob("fg_dump_d3d12_*.ppm"):
        m = NAME.match(path.name)
        if m:
            frames.setdefault(int(m.group(1)), {})[m.group(3)] = path
    if not frames:
        print("no dumps")
        return 1
    ids = sorted(frames)
    skipped = sum(1 for i in ids if "real_only" in frames[i])
    print(f"{len(ids)} frames dumped ({ids[0]}..{ids[-1]}), {skipped} without generation")
    rows = []
    for k in ids:
        f = frames[k]
        prev = frames.get(k - 1, {})
        prev_real = prev.get("real") or prev.get("real_only")
        if "generated" not in f or "real" not in f or not prev_real:
            continue
        g = read_ppm(f["generated"])
        r1 = read_ppm(f["real"])
        r0 = read_ppm(prev_real)
        # Generated at the size shown when the window is smaller: the real frames scaled to it
        # as the presenter scales them (bilinear, sampled at pixel centres), at any ratio.
        if g.shape != r1.shape:
            r1, r0 = bilinear(r1, g.shape), bilinear(r0, g.shape)
        g, r1, r0 = (downscale(x, args.downscale) for x in (g, r1, r0))
        if g.shape != r1.shape or r0.shape != r1.shape:
            continue
        still = np.all(r0 == r1, axis=2)
        still_ok = np.all(np.abs(g - r1) <= 2, axis=2) & still
        lo = window(np.minimum(r0, r1), args.window, np.minimum) - 8
        hi = window(np.maximum(r0, r1), args.window, np.maximum) + 8
        outside = np.any((g < lo) | (g > hi), axis=2)
        mean = float(np.mean(np.abs(g - (r0 + r1) / 2.0)))
        still_share = still_ok.sum() / max(1, still.sum())
        rows.append((k, still.mean(), still_share, outside.mean(), mean))
        if args.crops and outside.mean() > 0.001:
            ys, xs = np.nonzero(outside)
            cy, cx = int(np.median(ys)), int(np.median(xs))
            y0, x0 = max(0, cy - 64), max(0, cx - 64)
            try:
                from PIL import Image
                out = Path(args.crops)
                out.mkdir(parents=True, exist_ok=True)
                for name, img in (("r0", r0), ("g", g), ("r1", r1)):
                    crop = (img[y0:y0 + 128, x0:x0 + 128] >> 2).astype(np.uint8)
                    Image.fromarray(crop).resize((256, 256), Image.NEAREST).save(
                        out / f"crop_{k}_{name}.png")
            except ImportError:
                pass
    if not rows:
        print("no generated frame with both neighbours dumped")
        return 1
    print(" frame  still   still kept  outside  mean")
    for k, still, kept, outside, mean in rows:
        print(f"{k:6d}  {still:6.1%}  {kept:9.3%}  {outside:7.3%}  {mean:5.2f}")
    kept = [r[2] for r in rows]
    outside = [r[3] for r in rows]
    print(f"generated frames checked {len(rows)}: still pixels kept min {min(kept):.3%}, "
          f"outside median {np.median(outside):.3%} max {max(outside):.3%}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

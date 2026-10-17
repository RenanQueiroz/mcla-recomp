"""The HDR scene's curve before and after the #1 fix (phases 3-4 review), as
the shader computes it: Reinhard extended up to the knee, the exponential
shoulder above. After: the shoulder's slope is at least what reaches the
game's white at the game's white (s_min), and up to white the curve is never
below the game's; a peak at or below the game's white keeps the game's curve,
clipped at white. Per scene white (W = white^2), display levels and setting:
the worst shortfall against the game's curve between the knee and white, the
value at white, and where the curve reaches at 2x and 4x white's Ls.
usage: shoulder_fix.py"""
import math


def enc(x):
    return 1.055 * x ** (1 / 2.2) - 0.055 if x > 0.00304 else 12.92 * x


def srgb_eotf(c):
    return ((c + 0.055) / 1.055) ** 2.4 if c > 0.04045 else c / 12.92


def bt709_oetf(l):
    return 1.099 * l ** 0.45 - 0.099 if l >= 0.018 else 4.5 * l


TABLES = {
    "pc": [round(i * 1023 / 255) for i in range(256)],
    "tv": [round(bt709_oetf(srgb_eotf(i / 255)) * 1023) for i in range(256)],
}


def transfer(v, table):
    code = max(enc(v), 0) * 255
    i = int(min(math.floor(code), 254))
    lo, hi = table[i] / 1023, table[i + 1] / 1023
    return srgb_eotf(max(lo + (hi - lo) * (code - i), 0))


def peak_game(rel, table):
    lo, hi = 0.0, 64.0
    for _ in range(32):
        m = (lo + hi) / 2
        if transfer(m, table) > rel:
            hi = m
        else:
            lo = m
    return lo


def reinhard(ls, w):
    return ls * (1 + ls / w) / (1 + ls)


def setup(w, hl, peak, paper, table, knee=0.6, fixed=True):
    b = 1 - knee
    lsk = (-b + math.sqrt(b * b + 4 * knee / w)) * 0.5 * w
    s = ((1 + 2 * lsk / w) * (1 + lsk) - (lsk + lsk * lsk / w)) / ((1 + lsk) ** 2)
    p = peak_game(peak / paper, table)
    span = max(p - knee, 1e-3)
    slope = s * hl
    if fixed and span > 1 - knee:
        s_min = -span * math.log(1 - (1 - knee) / span) / (math.sqrt(w) - lsk)
        slope = max(slope, s_min)
    return knee, lsk, slope, span


def curve(ls, w, c, fixed=True):
    knee, lsk, slope, span = c
    ld = reinhard(ls, w)
    if ls > lsk:
        if not fixed or span > 1 - knee:
            sh = knee + span * (1 - math.exp(-(ls - lsk) * slope / span))
            ld = max(sh, ld) if fixed and ls < math.sqrt(w) else sh
        else:
            ld = min(ld, 1.0)
    return ld


def report(name, w, hl, peak, paper, table):
    out = []
    for fixed in (False, True):
        c = setup(w, hl, peak, paper, TABLES[table], fixed=fixed)
        lw = math.sqrt(w)
        worst = 0.0
        for k in range(1, 1001):
            ls = c[1] + (lw - c[1]) * k / 1000
            r = curve(ls, w, c, fixed) / reinhard(ls, w)
            worst = max(worst, 1 - r)
        vals = [curve(lw * f, w, c, fixed) for f in (1, 2, 4)]
        nits = [transfer(v, TABLES[table]) * paper for v in vals]
        out.append(f"{'after ' if fixed else 'before'} slope {c[2]:6.3f} worst {worst * 100:4.1f}% "
                   f"at 1x/2x/4x white: {vals[0]:.3f}/{vals[1]:.3f}/{vals[2]:.3f} "
                   f"({nits[0]:4.0f}/{nits[1]:4.0f}/{nits[2]:4.0f} nits)")
    print(f"{name:5s} W={w:5.3f} {peak:4d}/{paper:3d} {table} hl={hl}: " + "  |  ".join(out))


for table in ("tv", "pc"):
    for peak, paper in ((1317, 280), (600, 240), (400, 300), (280, 280)):
        for name, w in (("day", 0.335), ("dusk", 0.214), ("night", 2.25)):
            for hl in (1.0, 2.0):
                report(name, w, hl, peak, paper, table)
    print()

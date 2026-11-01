r"""Checks the log of a run of launcher/dev/scripts/deadzone.pad (src/stick_deadzone.cpp).

  python -I deadzone_check.py <run folder or game.log> [--summary FILE]

The run is started with --mcla_stick_deadzone_log=true --mcla_menu_log_layout=true and the dev pad's synthetic device as
the controller. For each step of the script (a `log DZ n` line) this compares the last line the module logged with what
the dead zones must give, then the pause menu's rows: they set the options, and the CONTROLS page stays within its
width cap (750 menu units). What it cannot show: how it feels on a real stick. Exit 0 when every check passes.
"""
import argparse
import math
import pathlib
import re
import sys

STICK = re.compile(r"stick dead zone: left ([\d.]+)% \((-?\d+),(-?\d+)\) -> \((-?\d+),(-?\d+)\), right ([\d.]+)% \((-?\d+),(-?\d+)\) -> \((-?\d+),(-?\d+)\)")
AXIS_27 = int(0.27 * 32767.0)


def apply(x, y, percent):
    if percent <= 0:
        return x, y
    zone = min(percent, 50) / 100.0
    fx, fy = x / 32767.0, y / 32767.0
    length = math.hypot(fx, fy)
    if length <= zone:
        return 0, 0
    scale = (length - zone) / (1.0 - zone) / length
    clamp = lambda v: max(-32768, min(32767, round(v * scale * 32767.0)))
    return clamp(fx), clamp(fy)


# step -> left percent, lx in, right percent, rx in
STEPS = {
    1: (0, 0, 0, AXIS_27),
    2: (0, 0, 30, AXIS_27),
    3: (0, 0, 20, AXIS_27),
    4: (0, 0, 20, 32767),
    5: (30, AXIS_27, 0, AXIS_27),
    6: (30, 32767, 0, AXIS_27),
    7: (0, AXIS_27, 0, AXIS_27),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--summary", default="")
    a = ap.parse_args()
    p = pathlib.Path(a.path)
    if p.is_dir():
        p = p / "game.log"
    last = {}
    menu = []
    widths = []
    step = 0
    errors = 0
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r"dev pad: +DZ (\d+) ", line)
        if m:
            step = int(m.group(1))
            continue
        m = STICK.search(line)
        if m and step:
            last[step] = [float(m.group(1))] + [int(v) for v in m.groups()[1:5]] + [float(m.group(6))] + [int(v) for v in m.groups()[6:]]
            continue
        m = re.search(r"settings menu: (mcla_(?:left|right)_stick_deadzone) = ([\d.]+)", line)
        if m:
            menu.append((m.group(1), float(m.group(2))))
            continue
        m = re.search(r"settings menu: 'CONTROLS' layout: menu.width (\d+) height (\d+) count (\d+)", line)
        if m:
            widths.append((int(m.group(1)), int(m.group(3))))
        if "[error]" in line or "[critical]" in line:
            errors += 1
    out = []

    def check(ok, text):
        out.append(f"{'PASS' if ok else 'FAIL'} {text}")
        return ok

    good = True
    for s, (lz, lx, rz, rx) in STEPS.items():
        got = last.get(s)
        want_l = apply(lx, 0, lz)
        want_r = apply(rx, 0, rz)
        ok = bool(got) and got[0] == lz and got[1] == lx and (got[3], got[4]) == want_l and got[5] == rz and got[6] == rx and (got[8], got[9]) == want_r
        good &= check(ok, f"step {s}: left {lz}% {lx} -> {want_l[0]}, right {rz}% {rx} -> {want_r[0]}; logged {got}")
    good &= check(("mcla_left_stick_deadzone", 10.0) in menu, f"the left row reached 10: {menu}")
    good &= check(("mcla_right_stick_deadzone", 30.0) in menu, "the right row reached 30")
    got = last.get(11)
    good &= check(bool(got) and got[5] == 30 and got[6] == AXIS_27 and got[8] == 0, f"after the menu a right stick at 0.27 is held by its 30%: {got}")
    good &= check(bool(widths), "the CONTROLS page's layout was logged")
    for w in sorted(set(widths)):
        good &= check(w[0] <= 750, f"CONTROLS page: menu.width {w[0]} with {w[1]} rows (cap 750)")
    good &= check(errors == 0, f"{errors} error lines")
    text = "\n".join(out)
    print(text)
    if a.summary:
        pathlib.Path(a.summary).write_text(text + "\n", encoding="utf-8")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())

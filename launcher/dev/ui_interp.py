r"""Check of the interpolated UI (mcla_ui_interpolate, development spike; fork-only development tool).

  ui_interp.py pad <out.pad> --shots <folder>      write the dev pad script of the run
  ui_interp.py report <run folder> [--out FILE] [--shots <folder>] [--plot FILE]
                                                   read the run's game.log (and pictures) and say what the option did

The run (devrun.py, a scratch save, --mcla_ui_interp_log=true --mcla_ui_advance_log=true) goes to the city, then:
  hud    the HUD at 30 steps a second (mcla_ui_hud_rate 30, the console's pace), cap 120: full throttle for 5 s and
         braking for 2.5 s, with the option off and then on
  pause  the pause menu's Settings tab, cap 120, option off and then on: 4 taps down and 4 up (600 ms apart), a hold
         of 1 s, 1.5 s at rest and a picture, and a burst of four pictures after a press
  open   the pause menu closed and opened four times (off, on, off, on): the animation's length and the steps it takes
  caps   the same taps at caps 60 and 30 with the option on (it must be applied at 60 and left off at 30)
and the game logs, for every drawn frame and every display-list element that glides (its shown position differs
from the step's), `ui interp: <screen> inst=.. ser=<step> t=<ms> alpha=.. live=(x,y) shown=(x,y) world=(x,y)
applied=0|1`: with the option off `shown` is what would have been drawn and `world` what is (the step's value).
The report picks the element that moves most in each cell and counts, per step of the movie, how many different
positions the drawn frames had: one with the option off (a staircase), several with it on.
"""
import argparse
import re
import statistics
import sys
from pathlib import Path

PAUSE_TAPS = [60, 100, 150, 80]


def _boot(add):
    add("wait 8s")
    add("mark title")
    add("press_until start left_title every=2s timeout=90s")
    add("mark map_zoom")
    add("wait_until roam 180s")
    add("wait 8s")
    add("mark free_roam")


def pad_lines(shots):
    out = []
    add = out.append
    add("# Made by ui_interp.py (see its docstring). Do not edit: change the generator.")
    _boot(add)
    add("set mcla_fps 120")
    add("set mcla_ui_timing_fix true")
    add("set mcla_ui_hud_rate 30")
    add("set mcla_ui_interp_window_pct 100")
    add("set mcla_ui_interp_log true")
    add("get mcla_ui_interpolate mcla_ui_interp_snap_px mcla_ui_menu_rate mcla_ui_hud_rate mcla_ui_advance_on_input")
    # the HUD, in the city: the needles and the ticker move
    for variant in ("off", "on"):
        add(f"set mcla_ui_interpolate {'true' if variant == 'on' else 'false'}")
        add("wait 800ms 96f")
        add(f"log UI cell=hud variant={variant} cap=120")
        add("trigger rt 1.0 5s")
        add("trigger lt 1.0 2500ms")
        add("log UI end")
        add("wait 1500ms 180f")
    # the pause menu's Settings tab, as the other menu checks reach it
    add("log UI setup pause")
    add("press start 4f")
    add("wait 1500ms 90f")
    for _ in range(2):
        add("press rb 4f")
        add("wait 1000ms 60f")
    add("wait_until ui_count >= 0 20s")
    add("uisel")

    def reset():
        add("press_until up ui_select == 0 every=500ms hold=50ms timeout=40s")
        add("wait 400ms")

    def taps(cap, variant, with_hold):
        add(f"set mcla_ui_interpolate {'true' if variant == 'on' else 'false'}")
        add(f"set mcla_fps {cap}")
        add("wait 800ms 96f")
        reset()
        add(f"log UI cell=pause variant={variant} cap={cap}")
        add("log UI phase=taps")
        for i, length in enumerate(PAUSE_TAPS):
            add(f"log UI tap down i={i}")
            add(f"press down {length}ms")
            add("wait 600ms")
        for i, length in enumerate(PAUSE_TAPS):
            add(f"log UI tap up i={i}")
            add(f"press up {length}ms")
            add("wait 600ms")
        if with_hold:
            add("log UI phase=hold")
            add("press down 1000ms")
            add("wait 700ms")
            reset()
            add("log UI phase=rest")
            add("wait 1500ms")
            add(f"shot {shots}/rest-{variant}.png 0.5")
            add("log UI phase=burst")
            add("down down")
            for i in range(4):
                add(f"shot {shots}/burst-{variant}-{i}.png 0.35")
            add("up down")
            add("wait 600ms")
            reset()
        add("log UI end")

    taps(120, "off", True)
    taps(120, "on", True)
    # the pause menu's opening and closing: Start closes it, Start opens it (the animation of the movie)
    add("set mcla_fps 120")
    add("wait 600ms")
    for k, variant in enumerate(("off", "on", "off", "on")):
        add(f"set mcla_ui_interpolate {'true' if variant == 'on' else 'false'}")
        reset()
        add("press start 100ms")
        add("wait 3000ms")
        add(f"log UI cell=open variant={variant} cap=120 id={k}")
        add("press start 100ms")
        add("wait 3000ms")
        add("log UI end")
    # the caps: the option on at 60 (applied) and 30 (left off: the frame is longer than 0.6 of a step)
    for cap in (60, 30):
        taps(cap, "on", False)
    add("set mcla_fps 120")
    add("set mcla_ui_interpolate false")
    add("mark end")
    return out


# --- report ---

LOG_LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})\] \[\w+\] \[[^\]]*\] \[[^\]]*\] (.*)$")
RE_MARK = re.compile(r"dev pad:\s+UI (.*)$")
RE_INTERP = re.compile(
    r"ui interp: (\S+) inst=([0-9A-F]{8}) d=(\d+) ser=(\d+) t=(\d+)\.(\d{3}) ms alpha=([\d.]+) "
    r"live=\(([-\d.]+),([-\d.]+)\) shown=\(([-\d.]+),([-\d.]+)\) world=\(([-\d.]+),([-\d.]+)\) applied=(\d)")
RE_CHECK = re.compile(r"ui interp check: nodes=(\d+) matrix_off=(\d+) \(worst ([\d.]+)\) cxform_off=(\d+) gliding=(\d+) applied=(\d+)")
RE_STEP = re.compile(r"ui interp step: (\S+) ser=(\d+) moved=(\d+) cx=(\d+) fresh=(\d+) snapped=(\d+) gap=(\d+) max_move=([\d.]+)px tracked=(\d+)")
RE_MOVIE = re.compile(r"ui interp movie: (\S+) ctx=([0-9A-F]{8}) swf_fps=(\d+) period=(\d+) us")
RE_ADV = re.compile(r"ui adv: (\S+) ctx=([0-9A-F]{8}) t=(\d+)\.(\d{3}) ms in=(\d)")


def _kv(text):
    return dict(p.split("=", 1) for p in text.split() if "=" in p)


def parse(text):
    """cells: list of {kind, variant, cap, id, phases: {phase: [interp rows]}, adv: [(name, ms)], lines}; checks: [..]."""
    cells = []
    cur = None
    phase = "all"
    checks = []
    movies = {}
    for line in text.splitlines():
        m = LOG_LINE.match(line)
        body = m.group(6) if m else line
        mm = RE_MARK.search(body)
        if mm:
            words = mm.group(1)
            if words.startswith("cell="):
                kv = _kv(words)
                cur = {"kind": kv.get("cell"), "variant": kv.get("variant"), "cap": int(kv.get("cap", 0)), "id": kv.get("id"),
                       "phases": {}, "adv": [], "taps": [], "census": []}
                cells.append(cur)
                phase = "all"
            elif words.startswith("phase="):
                phase = words.split("=", 1)[1]
            elif words.startswith("tap "):
                if cur is not None:
                    cur["taps"].append(words)
            elif words.startswith("end"):
                cur = None
            continue
        mi = RE_INTERP.search(body)
        if mi and cur is not None:
            g = mi.groups()
            row = {"screen": g[0], "inst": g[1], "depth": int(g[2]), "ser": int(g[3]), "t": int(g[4]) + int(g[5]) / 1000.0,
                   "alpha": float(g[6]), "live": (float(g[7]), float(g[8])), "shown": (float(g[9]), float(g[10])),
                   "world": (float(g[11]), float(g[12])), "applied": int(g[13])}
            cur["phases"].setdefault(phase, []).append(row)
            continue
        mmv = RE_MOVIE.search(body)
        if mmv:
            movies[mmv.group(1)] = (int(mmv.group(3)), int(mmv.group(4)))
            continue
        mst = RE_STEP.search(body)
        if mst and cur is not None:
            g = mst.groups()
            cur["census"].append({"screen": g[0], "ser": int(g[1]), "moved": int(g[2]), "cx": int(g[3]), "fresh": int(g[4]),
                                  "snapped": int(g[5]), "gap": int(g[6]), "max_move": float(g[7]), "tracked": int(g[8])})
            continue
        ma = RE_ADV.search(body)
        if ma and cur is not None:
            cur["adv"].append((ma.group(1), int(ma.group(3)) + int(ma.group(4)) / 1000.0))
            continue
        mc = RE_CHECK.search(body)
        if mc:
            checks.append(tuple(int(x) if i != 2 else float(x) for i, x in enumerate(mc.groups())))
    return cells, checks, movies


def movers(rows):
    """The elements that move most in these rows, by the path their world position takes: [(path, screen, inst, rows)]."""
    by = {}
    for r in rows:
        by.setdefault((r["screen"], r["inst"]), []).append(r)
    out = []
    for (screen, inst), rs in by.items():
        rs.sort(key=lambda r: r["t"])
        path = sum(abs(b["world"][0] - a["world"][0]) + abs(b["world"][1] - a["world"][1]) for a, b in zip(rs, rs[1:]))
        out.append((path, screen, inst, rs))
    out.sort(key=lambda x: -x[0])
    return out


def per_step(rs):
    """For each step (ser) of an element: (draws, distinct world positions). Steps with fewer than 3 draws are left out."""
    steps = {}
    for r in rs:
        steps.setdefault(r["ser"], []).append(r)
    res = []
    for ser, items in sorted(steps.items()):
        if len(items) >= 3:
            res.append((ser, len(items), len({(round(i["world"][0], 2), round(i["world"][1], 2)) for i in items})))
    return res


def median(values, default=0):
    return statistics.median(values) if values else default


def describe_cell(cell, lines):
    rows = [r for ph in cell["phases"].values() for r in ph]
    head = f"== {cell['kind']} variant {cell['variant']} cap {cell['cap']}" + (f" id {cell['id']}" if cell["id"] else "")
    lines.append(head)
    if not rows:
        lines.append("   no gliding element logged (nothing moved, or the hook did not run)")
        return None
    applied = sum(r["applied"] for r in rows)
    lines.append(f"   {len(rows)} logged draws of gliding elements, {applied} of them applied (applied=1)")
    ms = movers(rows)[:3]
    result = None
    for path, screen, inst, rs in ms:
        steps = per_step(rs)
        dts = [b["t"] - a["t"] for a, b in zip(rs, rs[1:]) if b["t"] - a["t"] < 40]
        lines.append(f"   mover {screen} {inst} (depth {rs[0]['depth']}): path {path:.1f} px over {len(rs)} draws; "
                     f"steps with 3+ draws: {len(steps)}; draws per step median {median([s[1] for s in steps]):.0f}; "
                     f"different world positions per step median {median([s[2] for s in steps]):.0f} (max {max([s[2] for s in steps] or [0])}); "
                     f"draw interval median {median(dts):.2f} ms")
        if result is None and steps:
            result = (screen, inst, median([s[2] for s in steps]), rs)
    # what each step changed (the census the draw hook keeps with mcla_ui_interp_log)
    by = {}
    for c in cell["census"]:
        by.setdefault(c["screen"], []).append(c)
    for screen, cs in sorted(by.items()):
        moving = [c for c in cs if c["moved"] or c["cx"] or c["fresh"]]
        if not moving:
            continue
        lines.append(f"   census {screen}: {len(cs)} steps, {len(moving)} with changes; per step with changes: moved median "
                     f"{median([c['moved'] for c in moving]):.0f} (max {max(c['moved'] for c in moving)}), colour transform {median([c['cx'] for c in moving]):.0f}, "
                     f"new or another character {median([c['fresh'] for c in moving]):.0f} (max {max(c['fresh'] for c in moving)}), "
                     f"snapped {sum(c['snapped'] for c in moving)} in all, steps missed {sum(c['gap'] for c in moving)}, "
                     f"largest move {max(c['max_move'] for c in moving):.1f} px, nodes tracked {median([c['tracked'] for c in moving]):.0f}")
    return result


def ascii_steps(rs, n=14):
    """The first full step of the element with at least 3 draws: a line per draw."""
    steps = {}
    for r in rs:
        steps.setdefault(r["ser"], []).append(r)
    for ser, items in sorted(steps.items()):
        if len(items) >= 4 and len({round(i["world"][1], 2) for i in items} | {round(i["world"][0], 2) for i in items}) >= 1:
            t0 = items[0]["t"]
            return [f"      step {ser}: " + ", ".join(f"+{i['t'] - t0:.1f}ms ({i['world'][0]:.1f},{i['world'][1]:.1f})" for i in items[:n])]
    return []


def plot(series, path, title):
    """series: [(label, rows)] -> a PNG: world y (and x if y does not move) against time, one panel per label."""
    from PIL import Image, ImageDraw
    W, H, M = 1100, 260, 40
    img = Image.new("RGB", (W, H * len(series)), (255, 255, 255))
    d = ImageDraw.Draw(img)
    for n, (label, rs) in enumerate(series):
        y0 = n * H
        d.text((M, y0 + 6), f"{title}: {label}", fill=(0, 0, 0))
        if not rs:
            d.text((M, y0 + 30), "no data", fill=(200, 0, 0))
            continue
        ys_ = [r["world"][1] for r in rs]
        xs_ = [r["world"][0] for r in rs]
        use_y = (max(ys_) - min(ys_)) >= (max(xs_) - min(xs_))
        vals = ys_ if use_y else xs_
        lo, hi = min(vals), max(vals)
        t0, t1 = rs[0]["t"], rs[-1]["t"]
        span = max(t1 - t0, 1e-6)
        rng = max(hi - lo, 1e-6)
        pts = [(M + (r["t"] - t0) / span * (W - 2 * M), y0 + H - M - ((v - lo) / rng) * (H - 2 * M)) for r, v in zip(rs, vals)]
        d.line([(M, y0 + H - M), (W - M, y0 + H - M)], fill=(160, 160, 160))
        for a, b in zip(pts, pts[1:]):
            d.line([a, b], fill=(120, 160, 230))
        for p in pts:
            d.ellipse([p[0] - 2, p[1] - 2, p[0] + 2, p[1] + 2], fill=(200, 40, 40))
        d.text((M, y0 + H - M + 6),
               f"{label}: {len(rs)} draws, {'y' if use_y else 'x'} {lo:.1f}..{hi:.1f} px over {span:.0f} ms (one dot = one drawn frame)", fill=(0, 0, 0))
    img.save(path)


def report(folder, out=None, shots=None, plot_path=None, echo=True):
    folder = Path(folder)
    log = folder / "game.log"
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    cells, checks, movies = parse(text)
    lines = [f"ui_interp report of {folder}", f"{len(cells)} cells, {sum(len(r) for c in cells for r in c['phases'].values())} interp rows, {len(checks)} self-check lines"]
    lines.append("movies seen drawn (the SWF's own frame rate, the pace they are stepped at): " + (", ".join(
        f"{n} swf {fps}/s stepped every {per / 1000:.1f} ms" for n, (fps, per) in sorted(movies.items())) or "none"))
    verdict = []
    results = {}
    for cell in cells:
        res = describe_cell(cell, lines)
        results[(cell["kind"], cell["variant"], cell["cap"], cell["id"])] = res
        if res and cell["kind"] in ("pause", "hud"):
            lines += ascii_steps(res[3])
    # self-check of the arithmetic
    if checks:
        last = checks[-1]
        ok = last[1] == 0 and last[3] == 0
        verdict.append(f"{'PASS' if ok else 'FAIL'} arithmetic: {last[0]} nodes checked, matrix differs from the game's world matrix in {last[1]} "
                       f"(worst {last[2]:.5f}), colour transform in {last[3]}")
    else:
        verdict.append("FAIL arithmetic: no `ui interp check` line in the log (the hook did not run, or the option for logging was not set)")
    # smoothness at cap 120
    for kind in ("pause", "hud"):
        off = results.get((kind, "off", 120, None))
        on = results.get((kind, "on", 120, None))
        if off and on:
            good = on[2] >= 3 and off[2] <= 1.5
            verdict.append(f"{'PASS' if good else 'CHECK'} smooth {kind}: different positions per step of the main mover: off {off[2]:.0f}, on {on[2]:.0f} "
                           f"({off[0]} {off[1]} / {on[0]} {on[1]})")
        else:
            verdict.append(f"CHECK smooth {kind}: no mover found in both variants")
    # applied at 60, not at 30
    for cap, want in ((60, True), (30, False)):
        cell = next((c for c in cells if c["kind"] == "pause" and c["cap"] == cap and c["variant"] == "on"), None)
        if cell is None:
            verdict.append(f"CHECK cap {cap}: cell missing")
            continue
        rows = [r for ph in cell["phases"].values() for r in ph]
        applied = sum(r["applied"] for r in rows)
        ok = (applied > 0) if want else (applied == 0)
        verdict.append(f"{'PASS' if ok else 'FAIL'} cap {cap}: {applied} of {len(rows)} gliding draws applied (expected {'some' if want else 'none'})")
    # the animation is as long: steps and time of the openings
    opens = [c for c in cells if c["kind"] == "open"]
    stats = {}
    for c in opens:
        rows = [r for ph in c["phases"].values() for r in ph]
        adv = [t for n, t in c["adv"] if n == "PAUSEMOVIE"]
        if rows:
            dur = max(r["t"] for r in rows) - min(r["t"] for r in rows)
            stats.setdefault(c["variant"], []).append((dur, len(adv), max(r["ser"] for r in rows) - min(r["ser"] for r in rows) + 1))
    for variant, items in stats.items():
        lines.append(f"open {variant}: " + "; ".join(f"{d:.0f} ms of gliding, {a} PAUSEMOVIE advances, {s} steps seen" for d, a, s in items))
    if "off" in stats and "on" in stats:
        d_off = median([x[0] for x in stats["off"]])
        d_on = median([x[0] for x in stats["on"]])
        a_off = median([x[1] for x in stats["off"]])
        a_on = median([x[1] for x in stats["on"]])
        ok = abs(a_on - a_off) <= 2 and abs(d_on - d_off) <= 60
        verdict.append(f"{'PASS' if ok else 'CHECK'} duration: opening animation {d_off:.0f} ms / {a_off:.0f} advances with the option off, "
                       f"{d_on:.0f} ms / {a_on:.0f} on")
    else:
        verdict.append("CHECK duration: the opening cells are missing")
    # pictures at rest
    if shots:
        try:
            from PIL import Image, ImageChops
            a = Image.open(Path(shots) / "rest-off.png").convert("RGB")
            b = Image.open(Path(shots) / "rest-on.png").convert("RGB")
            diff = ImageChops.difference(a, b).getbbox() if a.size == b.size else "size"
            verdict.append(f"{'PASS' if diff is None else 'CHECK'} rest: pictures at rest with the option off and on are "
                           f"{'identical' if diff is None else 'different (' + str(diff) + ')'}")
        except Exception as e:  # noqa: BLE001
            verdict.append(f"CHECK rest: could not compare the pictures ({e})")
    if plot_path:
        ser = []
        for variant in ("off", "on"):
            res = results.get(("pause", variant, 120, None))
            if res:
                inst = res[1]
                cell = next(c for c in cells if c["kind"] == "pause" and c["variant"] == variant and c["cap"] == 120)
                rs = [r for r in cell["phases"].get("taps", []) if r["inst"] == inst]
                ser.append((f"pause list, option {variant}, cap 120", rs[:200]))
        if ser:
            try:
                plot(ser, plot_path, f"world position of {results[('pause', 'off', 120, None)][1] if results.get(('pause', 'off', 120, None)) else '?'}")
                lines.append(f"plot: {plot_path}")
            except ImportError:
                lines.append("plot: not made (Pillow is missing: run the report with the scratchpad's upstream\bench-venv python)")
    lines.append("")
    lines.append("=== VERDICT")
    lines += verdict
    result = "\n".join(lines)
    if out:
        Path(out).write_text(result + "\n", encoding="utf-8")
    if echo:
        print(result)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pad")
    p.add_argument("out")
    p.add_argument("--shots", default="{SHOT}")
    r = sub.add_parser("report")
    r.add_argument("folder")
    r.add_argument("--out")
    r.add_argument("--shots")
    r.add_argument("--plot")
    a = ap.parse_args()
    if a.cmd == "pad":
        lines = pad_lines(a.shots)
        Path(a.out).write_text("\n".join(lines) + "\n", encoding="ascii")
        print(f"{a.out}: {len(lines)} lines")
        return 0
    report(a.folder, a.out, a.shots, a.plot)
    return 0


if __name__ == "__main__":
    sys.exit(main())

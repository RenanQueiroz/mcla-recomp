r"""Check of the interpolated pause-menu camera and 3D panels (mcla_ui_interp_camera; fork-only development tool).

  ui_camera.py pad <out.pad> --shots <folder>      write the dev pad script of the run
  ui_camera.py report <run folder> [--shots <folder>] [--out FILE]
                                                   read the run (devrun.py's folder: game*.log) and say what the option did

The run (devrun.py, a scratch save, D3D12, per-frame log on) goes to the city and then opens the pause menu, changes tab, enters a submenu and its
page and closes the menu again (Start, LB and RB, A and B), the option off, on, off, on at a 120 FPS cap and off, on at 60. Each of the four
moves is a cell (`copen`, `ctab`, `csub`, `cclose`); bursts of consecutive pictures of the opening, the submenu and the closing are cells of their own.

What the game logs (src/frame_timing.cpp, with mcla_ui_interp_log on):
  ui interp cam: <movie> ctx=.. ser=<step> t=<ms> ctrl=<blend the game got> live_ctrl=<blend the script set> fov=.. m2=(..) m3=(..)
      once per update while the menu's camera is wanted: the blend between the game's own view and the menu's (0 to 1), the field of view
      and two rows of the camera matrix (its direction and its place);
  ui interp3d: <movie> <property> ctx=.. obj=.. ser=<step> t=<ms> alpha=.. live=(..) shown=(..) applied=0|1
      every read of a property of the menu's 3D scene (camera: position, lookat, fovy; panels: trans, rot, scale, _alpha) that differed from what the
      script set in its last step: with the option off `shown` is what would have been drawn and the game gets `live`;
  ui interp event: <movie> kind=snap why=scene3d what=<property> ..       a change taken for a cut and shown at once.
"""
import argparse
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ui_interp as U  # noqa: E402

OFF_ON = ("off", "on")
STEP_MS = U.STEP_MS
KINDS = ("copen", "ctab", "csub", "cclose")
BURSTS = ("bopen", "bsub", "bclose")
NAMES = {"copen": "the menu opening", "ctab": "a tab change (LB then RB)", "csub": "a submenu and back (A then B)", "cclose": "the menu closing"}
SHOT_SCALE = "0.5"

# --- the pad ---


def _setopt(add, variant, cap):
    add(f"set mcla_fps {cap}")
    add(f"set mcla_ui_interpolate {'true' if variant == 'on' else 'false'}")


def _to_row0(add):
    add("press_until up ui_select == 0 every=400ms hold=40ms timeout=15s")
    add("wait 400ms")


def _moves(add, variant, cap, k):
    """The four moves of one round; the pause menu starts closed (in the city) and ends closed. It reopens on the tab it was left on (Settings),
    whose first row (OPTIONS) is a submenu; LB and RB leave the tab and come back."""
    tag = f"variant={variant} cap={cap} id={k}"
    _setopt(add, variant, cap)
    add("wait 1200ms 96f")
    add(f"log UI cell=copen {tag}")
    add("log UI phase=press")
    add("press start 100ms")
    add("wait 2500ms")
    add("log UI end")
    _to_row0(add)
    add(f"log UI cell=ctab {tag}")
    add("log UI phase=press")
    add("press lb 4f")
    add("wait 1600ms")
    add("press rb 4f")
    add("wait 1600ms")
    add("log UI end")
    _to_row0(add)
    add(f"log UI cell=csub {tag}")
    add("log UI phase=press")
    add("press a 4f")
    add("wait 1800ms")
    add("press b 4f")
    add("wait 1800ms")
    add("log UI end")
    _to_row0(add)
    add(f"log UI cell=cclose {tag}")
    add("log UI phase=press")
    add("press start 100ms")
    add("wait 2500ms")
    add("log UI end")
    add("wait 600ms")


def _bursts(add, shots, variant, n=20):
    """Pictures of consecutive swaps right after the press: the opening, the submenu, the closing (cap 120)."""
    tag = f"variant={variant} cap=120"
    _setopt(add, variant, 120)
    add("wait 1200ms 96f")
    add(f"log UI cell=bopen {tag}")
    add("log UI phase=press")
    add("press start 100ms")
    add(f"burst {shots}/bopen-{variant}-120 {n} {SHOT_SCALE}")
    add("wait 2500ms")
    add("log UI end")
    _to_row0(add)
    add(f"log UI cell=bsub {tag}")
    add("log UI phase=press")
    add("press a 4f")
    add(f"burst {shots}/bsub-{variant}-120 {n} {SHOT_SCALE}")
    add("wait 1500ms")
    add("press b 4f")
    add("wait 1800ms")
    add("log UI end")
    _to_row0(add)
    add(f"log UI cell=bclose {tag}")
    add("log UI phase=press")
    add("press start 100ms")
    add(f"burst {shots}/bclose-{variant}-120 {n} {SHOT_SCALE}")
    add("wait 2500ms")
    add("log UI end")
    add("wait 600ms")


def pad_lines(shots):
    out = []
    add = out.append
    add("# Made by ui_camera.py (see its docstring). Do not edit: change the generator.")
    U._boot(add)
    add("get mcla_ui_interpolate mcla_ui_interp_camera mcla_ui_timing_fix mcla_ui_advance_on_input")
    add("set mcla_ui_interp_camera true")
    add("set mcla_ui_interp_log true")
    # one visit of the pause menu to its Settings tab, so that it reopens there; then closed again
    add("log UI setup pause")
    add("press start 4f")
    add("wait 1500ms 90f")
    for _ in range(2):
        add("press rb 4f")
        add("wait 1000ms 60f")
    add("wait_until ui_count >= 0 20s")
    add("uisel")
    add("press start 100ms")
    add("wait 2500ms")
    k = 0
    for cap in (120, 60):
        for variant in OFF_ON * (2 if cap == 120 else 1):
            _moves(add, variant, cap, k)
            k += 1
    for variant in OFF_ON:
        _bursts(add, shots, variant)
    add("set mcla_ui_interp_log false")
    add("set mcla_fps 120")
    add("set mcla_ui_interpolate false")
    add("mark end")
    return out


# --- reading the log ---

RE_CAM = re.compile(r"ui interp cam: (\S+) ctx=([0-9A-F]{8}) ser=(\d+) t=(\d+)\.(\d{3}) ms ctrl=(-?[\d.]+) live_ctrl=(-?[\d.]+) fov=(-?[\d.]+) "
                    r"m2=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) m3=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\)")
RE_3D = re.compile(r"ui interp3d: (\S+) (\S+) ctx=([0-9A-F]{8}) obj=([0-9A-F]{8}) ser=(\d+) t=(\d+)\.(\d{3}) ms alpha=([\d.]+) "
                   r"live=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) shown=\((-?[\d.]+),(-?[\d.]+),(-?[\d.]+)\) applied=(\d)")


def parse(text):
    """-> cells, checks. A cell: kind, variant, cap, id, t0, t1 (the log's own clock, seconds of the day), phase_walls, cam [dict], s3d [dict],
    events [dict], bursts [dict]. Rows outside a cell are dropped."""
    cells, checks = [], []
    cur = None
    for line in text.splitlines():
        m = U.LOG_LINE.match(line)
        body = m.group(6) if m else line
        wall = int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4)) + int(m.group(5)) / 1000.0 if m else 0.0
        if "ui interp cam: " in body and cur is not None:
            mc = RE_CAM.search(body)
            if mc:
                g = mc.groups()
                cur["cam"].append({"wall": wall, "screen": g[0], "ser": int(g[2]), "t": int(g[3]) + int(g[4]) / 1000.0, "ctrl": float(g[5]),
                                   "live": float(g[6]), "fov": float(g[7]), "m2": tuple(float(x) for x in g[8:11]),
                                   "m3": tuple(float(x) for x in g[11:14])})
            continue
        if "ui interp3d: " in body and cur is not None:
            m3 = RE_3D.search(body)
            if m3:
                g = m3.groups()
                cur["s3d"].append({"wall": wall, "screen": g[0], "name": g[1], "obj": g[3], "ser": int(g[4]), "t": int(g[5]) + int(g[6]) / 1000.0,
                                   "alpha": float(g[7]), "live": tuple(float(x) for x in g[8:11]), "shown": tuple(float(x) for x in g[11:14]),
                                   "applied": int(g[14])})
            continue
        mm = U.RE_MARK.search(body)
        if mm:
            words = mm.group(1)
            if words.startswith("cell="):
                kv = U._kv(words)
                cur = {"kind": kv.get("cell"), "variant": kv.get("variant"), "cap": int(kv.get("cap", 0)), "id": kv.get("id"), "t0": wall, "t1": wall,
                       "phase_walls": {}, "cam": [], "s3d": [], "events": [], "bursts": []}
                cells.append(cur)
            elif words.startswith("phase=") and cur is not None:
                cur["phase_walls"][words.split("=", 1)[1]] = wall
            elif words.startswith("end") and cur is not None:
                cur["t1"] = wall
                cur = None
            continue
        mk = U.RE_CHECK.search(body)
        if mk:
            checks.append(tuple(int(x) if i != 2 else float(x) for i, x in enumerate(mk.groups())))
            continue
        if cur is None:
            continue
        me = U.RE_EVENT.search(body)
        if me and "scene3d" in me.group(3):
            ev = {"screen": me.group(1), "wall": wall}
            ev.update(U._kv(me.group(3)))
            cur["events"].append(ev)
            continue
        mb = U.RE_BURST.search(body)
        if mb:
            g = mb.groups()
            cur["bursts"].append({"prefix": g[0], "k": int(g[1]), "swap0": int(g[2]), "swap1": int(g[3]), "t": int(g[4]) + int(g[5]) / 1000.0})
    return cells, checks


# --- what the rows say ---


def _min_draws(cap):
    return max(2, int(cap / 30 * 0.75)) if cap > 0 else 3


def cam_state(r):
    return (round(r["ctrl"], 4), round(r["fov"], 3)) + tuple(round(x, 4) for x in r["m2"] + r["m3"])


def per_step_distinct(rows, key, need):
    """rows (in time order) -> (moving steps, [distinct states in each]): the steps in which the state differs from the last state of the step
    before and that have at least `need` rows."""
    by = defaultdict(list)
    for r in rows:
        by[r["ser"]].append(key(r))
    sers = sorted(by)
    out = []
    prev = None
    for s in sers:
        states = by[s]
        if prev is not None and states[-1] != prev and len(states) >= need:
            out.append(len(set(states)))
        prev = states[-1]
    return out


def camera_stats(cell):
    rows = sorted(cell["cam"], key=lambda r: r["t"])
    counts = per_step_distinct(rows, cam_state, _min_draws(cell["cap"]))
    return {"rows": len(rows), "moving_steps": len(counts), "median": statistics.median(counts) if counts else 0, "max": max(counts or [0])}


def panel_series(cell):
    """{(obj, name): [rows]} for the panels' properties (the camera's own are in the cam rows)."""
    by = defaultdict(list)
    for r in cell["s3d"]:
        if r["name"] in ("trans", "rot", "scale", "_alpha"):
            by[(r["obj"], r["name"])].append(r)
    return by


def panel_stats(cell):
    """Per property of each panel object that moves (at least 4 different step values): the different values drawn per step, median over its moving steps."""
    need = _min_draws(cell["cap"])
    per = []
    for (obj, name), rs in panel_series(cell).items():
        rs = sorted(rs, key=lambda r: r["t"])
        if len({r["live"] for r in rs}) < 4:
            continue
        counts = per_step_distinct(rs, lambda r: r["shown"] if r["applied"] else r["live"], need)
        if counts:
            per.append((obj, name, len(rs), len(counts), statistics.median(counts)))
    allc = [p[4] for p in per]
    return {"series": len(per), "median": statistics.median(allc) if allc else 0, "detail": sorted(per, key=lambda p: -p[2])[:4]}


def _first(rows, pred):
    for r in rows:
        if pred(r):
            return r
    return None


def timing(cell):
    """The camera's blend over a `copen` or `cclose` cell, in ms from the press: when it left 0 / reached 1 / came back to 0 as the script set it
    (live_ctrl) and as the game got it (ctrl)."""
    press = cell["phase_walls"].get("press", cell["t0"])
    rows = sorted((r for r in cell["cam"] if r["wall"] >= press - 0.01), key=lambda r: r["wall"])
    if not rows:
        return None
    ms = lambda r: (r["wall"] - press) * 1000.0  # noqa: E731
    out = {"rows": len(rows)}
    if cell["kind"] == "copen":
        a = _first(rows, lambda r: r["live"] > 0)
        b = _first(rows, lambda r: r["live"] >= 0.9999)
        c = _first(rows, lambda r: r["ctrl"] > 0)
        d = _first(rows, lambda r: r["ctrl"] >= 0.9999)
        out.update(live_start=ms(a) if a else None, live_full=ms(b) if b else None, shown_start=ms(c) if c else None, shown_full=ms(d) if d else None)
        out["live_span"] = out["live_full"] - out["live_start"] if a and b else None
        out["shown_span"] = out["shown_full"] - out["shown_start"] if c and d else None
    else:
        live_pos = [r for r in rows if r["live"] > 0]
        shown_pos = [r for r in rows if r["ctrl"] > 0]
        a = _first(rows, lambda r: r["live"] < 0.9999)
        out.update(live_start=ms(a) if a else None, live_end=ms(live_pos[-1]) if live_pos else None,
                   shown_end=ms(shown_pos[-1]) if shown_pos else None)
        out["live_span"] = out["live_end"] - out["live_start"] if a and live_pos else None
        out["shown_span"] = out["shown_end"] - out["live_start"] if a and shown_pos else None
    out["live_steps"] = len({r["ser"] for r in rows if 0 < r["live"] < 0.9999})
    return out


def _med(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def _f(x, digits=0):
    return "-" if x is None else f"{x:.{digits}f}"


def smooth_verdicts(cells, lines, verdict):
    for kind in KINDS:
        for cap in (120, 60):
            res = {}
            for v in OFF_ON:
                cs = [c for c in cells if c["kind"] == kind and c["cap"] == cap and c["variant"] == v]
                res[v] = (cs, [camera_stats(c) for c in cs], [panel_stats(c) for c in cs])
            if not res["off"][0] or not res["on"][0]:
                continue
            need_on = 3 if cap >= 100 else 2
            for what, idx in (("camera", 1), ("panels", 2)):
                med = {}
                moving = {}
                for v in OFF_ON:
                    stats = res[v][idx]
                    if what == "camera":
                        vals = [s["median"] for s in stats if s["moving_steps"]]
                        moving[v] = sum(s["moving_steps"] for s in stats)
                    else:
                        vals = [s["median"] for s in stats if s["series"]]
                        moving[v] = sum(s["series"] for s in stats)
                    med[v] = statistics.median(vals) if vals else None
                    for c, s in zip(res[v][0], stats):
                        if what == "camera":
                            lines.append(f"   {kind} cap {cap} {v:3s} id {c['id']}: camera: {s['rows']} update rows, {s['moving_steps']} steps in which it moved, "
                                         f"different states drawn per such step: median {s['median']:.0f}, max {s['max']}")
                        else:
                            d = "; ".join(f"{n} {o[-4:]}: {m:.0f} (over {k} steps)" for o, n, _, k, m in s["detail"])
                            lines.append(f"   {kind} cap {cap} {v:3s} id {c['id']}: panels: {s['series']} moving properties; different values drawn per step: "
                                         f"median {s['median']:.0f}" + (f" ({d})" if d else ""))
                tag = f"smooth {what} {kind} cap {cap}"
                if med["off"] is None and med["on"] is None:
                    verdict.append(f"CHECK {tag}: nothing of the {what} moved in {NAMES[kind]} (no step with a change)")
                elif med["off"] is None or med["on"] is None:
                    verdict.append(f"CHECK {tag}: the {what} moved with the option {'on' if med['off'] is None else 'off'} only "
                                   f"(off {moving['off']}, on {moving['on']} moving)")
                else:
                    ok = med["off"] <= 1.01 and med["on"] >= need_on
                    verdict.append(f"{'PASS' if ok else 'FAIL'} {tag}: different states drawn per 33 ms step during {NAMES[kind]}: "
                                   f"{med['off']:.0f} with the option off, {med['on']:.0f} on (needs 1 and at least {need_on})")


def transition_verdicts(cells, lines, verdict):
    for kind in ("copen", "cclose"):
        for cap in (120, 60):
            res = {}
            for v in OFF_ON:
                cs = [c for c in cells if c["kind"] == kind and c["cap"] == cap and c["variant"] == v]
                res[v] = [t for t in (timing(c) for c in cs) if t]
                for c, t in zip(cs, res[v]):
                    if kind == "copen":
                        lines.append(f"   {kind} cap {cap} {v:3s} id {c['id']}: the script's blend left 0 at {_f(t['live_start'])} ms and reached 1 at {_f(t['live_full'])} ms "
                                     f"({t['live_steps']} steps between); the game's: left 0 at {_f(t['shown_start'])} ms, reached 1 at {_f(t['shown_full'])} ms")
                    else:
                        lines.append(f"   {kind} cap {cap} {v:3s} id {c['id']}: the script's blend left 1 at {_f(t['live_start'])} ms and was last above 0 at "
                                     f"{_f(t['live_end'])} ms ({t['live_steps']} steps between); the game's was last above 0 at {_f(t['shown_end'])} ms")
            if not res["off"] or not res["on"]:
                continue
            tag = f"transition {kind} cap {cap}"
            frame = 1000.0 / cap
            tol = STEP_MS + frame
            # the movie's own length (equal by construction: same steps)
            sl_off, sl_on = _med([t["live_span"] for t in res["off"]]), _med([t["live_span"] for t in res["on"]])
            ss_off, ss_on = _med([t["shown_span"] for t in res["off"]]), _med([t["shown_span"] for t in res["on"]])
            st_off, st_on = _med([t["live_steps"] for t in res["off"]]), _med([t["live_steps"] for t in res["on"]])
            if None in (sl_off, sl_on, ss_off, ss_on):
                verdict.append(f"CHECK {tag}: no blend values in some cells (script's span off {sl_off}, on {sl_on}; game's off {ss_off}, on {ss_on})")
                continue
            ok_live = abs(sl_on - sl_off) <= STEP_MS and abs(st_on - st_off) <= 1.01
            ok_shown = abs(ss_on - ss_off) <= tol
            verdict.append(f"{'PASS' if ok_live else 'FAIL'} {tag} (the script's own): from the blend leaving its end to reaching the other "
                           f"{sl_off:.0f} ms with the option off, {sl_on:.0f} ms on ({st_off:.0f} and {st_on:.0f} steps; equal within one step, {STEP_MS:.0f} ms)")
            verdict.append(f"{'PASS' if ok_shown else 'FAIL'} {tag} (what the game got): {ss_off:.0f} ms with the option off, {ss_on:.0f} ms on "
                           f"(equal within one step and a frame, {tol:.0f} ms; the glide trails the script by up to one step by design)")


def event_verdicts(cells, lines, verdict):
    by = defaultdict(lambda: [0, None])
    for c in cells:
        for ev in c["events"]:
            e = by[(c["kind"], c["variant"], ev.get("what"))]
            e[0] += 1
            e[1] = e[1] or ev
    for (kind, variant, what), (n, ex) in sorted(by.items(), key=lambda x: str(x[0])):
        lines.append(f"   cut: {kind} {variant}: {what} {n} times, for example move {ex.get('move')} against the limit {ex.get('limit')}")
    total = sum(e[0] for e in by.values())
    verdict.append(f"NOTE cuts: {total} changes of the camera or the panels were taken for a cut and shown at once (limits: position and lookat 25, trans 8, rot 1.2, "
                   f"scale 0.6, fov 20, blend 0.5); the lines above say which; none is expected in these moves")


def arithmetic_verdict(checks, verdict):
    if not checks:
        verdict.append("FAIL arithmetic: no 'ui interp check' line in the log")
        return
    last = checks[-1]
    ok = last[1] == 0 and last[3] == 0 and last[0] > 0
    verdict.append(f"{'PASS' if ok else 'FAIL'} arithmetic: {last[0]} node draws compared with the game's own world matrices and colours, {last[1]} matrix and {last[3]} "
                   f"colour differences (worst {last[2]:.5f}); {last[4]} gliding, {last[5]} applied")


# --- pictures ---


def burst_check(shots, cells, lines, verdict, sheet_path=None):
    if not U._pil():
        lines.append("   (no PIL: pictures not looked at)")
        return
    from PIL import Image
    shots = Path(shots)
    rows, summary = [], {}
    for kind in BURSTS:
        for v in OFF_ON:
            prefix = f"{kind}-{v}-120"
            files = sorted(shots.glob(f"{prefix}-*.png"), key=lambda p: int(p.stem.rsplit("-", 1)[1]))
            if len(files) < 3:
                continue
            imgs = [Image.open(f).convert("RGB") for f in files]
            counts = [U._changed(a, b)[0] for a, b in zip(imgs, imgs[1:])]
            bursts = sorted((b for c in cells for b in c["bursts"] if Path(b["prefix"]).name == prefix), key=lambda b: b["k"])
            skipped = sum(max(0, b["swap0"] - a["swap1"] - 1) for a, b in zip(bursts, bursts[1:])) if bursts else None
            summary[prefix] = (counts, skipped, bursts)
            rows.append((prefix, imgs))
            lines.append(f"burst {prefix}: {len(imgs)} pictures, {skipped if skipped is not None else '?'} swaps skipped by the capture; changed pixels from one "
                         f"picture to the next: {counts}; {sum(1 for c in counts if c > 0)} of {len(counts)} pairs differ")
    for kind in BURSTS:
        off, on = summary.get(f"{kind}-off-120"), summary.get(f"{kind}-on-120")
        if off and on:
            m_off, m_on = sum(1 for c in off[0] if c > 0), sum(1 for c in on[0] if c > 0)
            verdict.append(f"{'PASS' if m_on > m_off else 'CHECK'} burst {kind}: consecutive pictures that differ: off {m_off} of {len(off[0])}, on {m_on} of {len(on[0])} "
                           f"(a staircase changes once per step, a glide on nearly every frame; the contact sheet is for the eyes)")
    if sheet_path and rows:
        U.contact_sheet(rows, sheet_path, tile_w=240)
        lines.append(f"   contact sheet: {sheet_path} (a row per burst, a column per consecutive picture)")


def report(folder, out=None, shots=None, echo=True):
    cells, checks = parse(U.read_log(folder))
    lines, verdict = [], []
    cams = sum(len(c["cam"]) for c in cells)
    rows3 = sum(len(c["s3d"]) for c in cells)
    lines.append(f"== {len(cells)} cells ({', '.join(sorted({c['kind'] for c in cells}))}); {cams} camera rows, {rows3} scene rows")
    if not cams:
        verdict.append("FAIL camera: no 'ui interp cam' row in the log (the hooks did not run, or the menu never asked for its camera)")
    arithmetic_verdict(checks, verdict)
    smooth_verdicts(cells, lines, verdict)
    transition_verdicts(cells, lines, verdict)
    event_verdicts(cells, lines, verdict)
    if shots and Path(shots).exists():
        burst_check(shots, cells, lines, verdict, str(Path(out).with_name("cam-bursts.png")) if out else None)
    text = "\n".join(lines + ["", "=== VERDICT"] + verdict) + "\n"
    if out:
        Path(out).write_text(text, encoding="utf-8")
    if echo:
        print(text)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pad")
    p.add_argument("out")
    p.add_argument("--shots", required=True)
    r = sub.add_parser("report")
    r.add_argument("folder")
    r.add_argument("--shots")
    r.add_argument("--out")
    a = ap.parse_args()
    if a.cmd == "pad":
        lines = pad_lines(a.shots)
        problems = U.lint(lines)
        if problems:
            raise SystemExit("pad lint: " + "; ".join(problems))
        Path(a.out).write_text("\n".join(lines) + "\n", encoding="ascii")
        print(f"{a.out}: {len(lines)} lines")
        return 0
    report(a.folder, a.out, a.shots)
    return 0


if __name__ == "__main__":
    sys.exit(main())

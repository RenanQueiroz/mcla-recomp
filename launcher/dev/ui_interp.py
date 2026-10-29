r"""Check of the interpolated UI (mcla_ui_interpolate, development spike; fork-only development tool).

  ui_interp.py pad <trace|cost|big> <out.pad> --shots <folder>   write the dev pad script of one run
  ui_interp.py report <out dir> [--shots <folder>] [--out FILE] [--plot FILE]
                                                               read the runs under <out dir> and say what the option did
                                                               (<out dir>\trace, \cost, \big: a run folder each, as devrun.py makes them;
                                                               <shots>\trace and \big hold the pictures)

The runs (devrun.py, a scratch save, D3D12) go to the title screen and the city, then:
  trace  (1x, per-frame log on) the title screen, the HUD (driving, with its ticker), the pause menu's opening and
         closing, its highlight moving (taps and a hold), pictures at rest and a burst of consecutive pictures, at cap
         120 and 60 with the option off and on, and at cap 30 with it on
  cost   (1x, logs off, --mcla_ui_interp_stats) the HUD and the pause menu, the option off, on, off, on, at cap 120 and
         uncapped: the hook's time, the process's CPU per frame and the frame times
  big    (3x resolution) bursts, then the same cost cells
The game logs, for every drawn frame and every display-list element that glides (its shown position differs from the
step's), `ui interp: <screen> inst=.. ser=<step> t=<ms> alpha=.. live=(x,y) shown=(x,y) world=(x,y) lm=.. sm=.. cx=..
applied=0|1`: with the option off `shown` is what would have been drawn and `world` what is (the step's value). The game
rotates its log files (5 MB each unless --log_max_file_size_mb says more): read_log() reads them all.
"""
import argparse
import re
import statistics
import sys
from collections import defaultdict, namedtuple
from pathlib import Path

PAUSE_TAPS = [60, 100, 150, 80]
OFF_ON = ("off", "on")
# the part of the frame a burst keeps (fractions): the pause list's rows; the HUD's ticker line
CROP_LIST = (0.30, 0.34, 0.66, 0.64)
CROP_HUD = (0.45, 0.78, 1.00, 0.97)
HUD_SCREENS = {"HUDGAUGE", "HUDMAPMOVIE", "SIDEKICKMOVIE"}

# --- the pad scripts ---


def _setopt(add, variant, cap):
    add(f"set mcla_fps {cap}")
    add(f"set mcla_ui_interpolate {'true' if variant == 'on' else 'false'}")


def _boot(add, title_cells=False):
    add("wait 8s")
    add("mark title")
    add("set mcla_ui_timing_fix true")
    add("set mcla_ui_menu_rate 30")
    add("set mcla_ui_hud_rate 30")
    add("set mcla_ui_interp_window_pct 100")
    if title_cells:
        add("get mcla_ui_interpolate mcla_ui_interp_snap_px mcla_ui_menu_rate mcla_ui_hud_rate mcla_ui_advance_on_input")
        add("screens")
        for cap in (120, 60):
            for variant in OFF_ON:
                _setopt(add, variant, cap)
                add("wait 700ms")  # time only: a wait for engine frames has no timeout, and the title's frames are not known to count
                add(f"log UI cell=title variant={variant} cap={cap}")
                add("wait 3s")
                add("log UI end")
        add("screens")
    add("set mcla_fps 120")
    add("set mcla_ui_interpolate false")
    add("press_until start left_title every=2s timeout=90s")
    add("mark map_zoom")
    add("wait_until roam 180s")
    add("wait 8s")
    add("mark free_roam")


def _open_pause(add):
    """The pause menu on its Settings tab (as the menu-lag and menu-input checks reach it)."""
    add("log UI setup pause")
    add("press start 4f")
    add("wait 1500ms 90f")
    for _ in range(2):
        add("press rb 4f")
        add("wait 1000ms 60f")
    add("wait_until ui_count >= 0 20s")
    add("uisel")


def _reset(add):
    add("press_until up ui_select == 0 every=500ms hold=50ms timeout=40s")
    add("wait 400ms")


def _hud_burst(add, shots, scale):
    """A burst of consecutive pictures of the ticker line (the car stands still), the option off and on."""
    for variant in OFF_ON:
        _setopt(add, variant, 120)
        add("wait 800ms 60f")
        add(f"log UI cell=hudburst variant={variant} cap=120")
        add(f"burst {shots}/hud-{variant}-120 12 {scale} " + " ".join(str(x) for x in CROP_HUD))
        add("wait 300ms")
        add("log UI end")


def _hud_cells(add, shots):
    for cap in (120, 60):
        for variant in OFF_ON:
            _setopt(add, variant, cap)
            add("wait 800ms 60f")
            add(f"log UI cell=hud variant={variant} cap={cap}")
            add("trigger rt 1.0 5s")
            add("trigger lt 1.0 2500ms")
            add("log UI end")
            add("wait 1500ms 90f")
    # the rates at their defaults: with the option on every movie steps 30 times a second, whatever the rate says
    add("set mcla_ui_hud_rate 60")
    for variant in OFF_ON:
        _setopt(add, variant, 120)
        add("wait 800ms 60f")
        add(f"log UI cell=hudrate variant={variant} cap=120")
        add("wait 4s")
        add("log UI end")
    add("set mcla_ui_hud_rate 30")
    _hud_burst(add, shots, 1.0)


def _taps(add, shots, cap, variant, full, scale, with_rest=True):
    _setopt(add, variant, cap)
    add("wait 800ms 96f")
    _reset(add)
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
    if full:
        add("log UI phase=hold")
        add("press down 1000ms")
        add("wait 700ms")
        _reset(add)
        if with_rest:
            add("log UI phase=rest")
            add("wait 1500ms")
            add(f"shot {shots}/rest-{variant}-{cap}-0.png 0.5")
            add("wait 700ms")
            add(f"shot {shots}/rest-{variant}-{cap}-1.png 0.5")
            add("wait 1100ms")
            add(f"shot {shots}/rest-{variant}-{cap}-2.png 0.5")
        add("log UI phase=burst")
        add("down down")
        add(f"burst {shots}/burst-{variant}-{cap} 12 {scale} " + " ".join(str(x) for x in CROP_LIST))
        add("up down")
        add("wait 600ms")
        _reset(add)
    add("log UI end")


def _open_close(add, cap, variant, k):
    _setopt(add, variant, cap)
    _reset(add)
    add(f"log UI cell=close variant={variant} cap={cap} id={k}")
    add("press start 100ms")
    add("wait 3000ms")
    add("log UI end")
    add(f"log UI cell=open variant={variant} cap={cap} id={k}")
    add("press start 100ms")
    add("wait 3000ms")
    add("log UI end")


def _cost_cells(add, scene, caps, seconds):
    """The option off, on, off, on at each cap: a mark (its own frame-time segment) and a cell per run; the settling
    time and the mark that ends a cell are outside the segment."""
    for cap in caps:
        for k, variant in enumerate(OFF_ON * 2):
            _setopt(add, variant, cap)
            add("wait 1000ms 60f")
            name = f"c-{scene}-{cap}-{variant}-{k}"
            add(f"mark {name}")
            add(f"log UI cell=cost variant={variant} cap={cap} scene={scene} id={k} mark={name}")
            add(f"wait {seconds}s")
            add("log UI end")
            add("mark gap")


def pad_lines(kind, shots):
    out = []
    add = out.append
    add(f"# Made by ui_interp.py (see its docstring), kind {kind}. Do not edit: change the generator.")
    if kind == "trace":
        _boot(add, title_cells=True)
        add("get mcla_ui_interpolate mcla_ui_interp_snap_px mcla_ui_menu_rate mcla_ui_hud_rate mcla_ui_advance_on_input")
        _hud_cells(add, shots)
        _open_pause(add)
        for k, variant in enumerate(OFF_ON * 2):
            _open_close(add, 120, variant, k)
        for k, variant in enumerate(OFF_ON):
            _open_close(add, 60, variant, 4 + k)
        for variant in OFF_ON:
            _taps(add, shots, 120, variant, True, 1.0)
        for variant in OFF_ON:
            _taps(add, shots, 60, variant, True, 1.0, with_rest=False)
        _taps(add, shots, 30, "on", False, 1.0)
        # a probe for a pop-up: the first tab's first row
        _setopt(add, "on", 120)
        add("log UI cell=probe variant=on cap=120")
        add("press lb 4f")
        add("wait 1000ms 60f")
        add("press lb 4f")
        add("wait 1000ms 60f")
        add("screens")
        add("press a 4f")
        add("wait 2000ms 120f")
        add("screens")
        add(f"shot {shots}/probe.png 0.5")
        add("log UI end")
    elif kind == "cost":
        _boot(add)
        add("set mcla_ui_interp_stats true")
        _cost_cells(add, "hud", (120, 0), 10)
        _open_pause(add)
        _cost_cells(add, "pause", (120, 0), 10)
    elif kind == "big":
        # the interpolation at 3x resolution: bursts with the log on, then the cost cells with it off
        _boot(add)
        add("set mcla_ui_interp_log true")
        _hud_burst(add, shots, 0.5)
        _open_pause(add)
        for variant in OFF_ON:
            _taps(add, shots, 120, variant, True, 0.5, with_rest=False)
        add("set mcla_ui_interp_log false")
        add("set mcla_ui_interp_stats true")
        _cost_cells(add, "pause", (120, 0), 8)
        add("press start 100ms")
        add("wait 2500ms")
        _cost_cells(add, "hud", (120, 0), 8)
    else:
        raise SystemExit(f"unknown pad kind {kind!r}")
    add("set mcla_fps 120")
    add("set mcla_ui_interpolate false")
    add("mark end")
    return out


KNOWN = {"press", "hold", "down", "up", "release", "trigger", "stick", "wait", "wait_until", "press_until", "mark", "shot",
         "burst", "set", "get", "log", "screens", "uisel", "state", "quit"}


def lint(lines):
    """Command names and argument counts the game's pad parser takes (src/dev_pad.cpp), for the lines of a generated script."""
    problems = []
    for n, line in enumerate(lines, 1):
        words = line.split()
        if not words or words[0].startswith("#"):
            continue
        cmd = words[0]
        if cmd not in KNOWN:
            problems.append(f"line {n}: unknown command {cmd!r}")
        elif cmd == "burst" and len(words) not in (3, 4, 8):
            problems.append(f"line {n}: burst takes <prefix> <n> [scale] [x0 y0 x1 y1]: {line}")
        elif cmd == "set" and len(words) != 3:
            problems.append(f"line {n}: set takes <option> <value>: {line}")
        elif cmd == "mark" and len(words) != 2:
            problems.append(f"line {n}: mark takes one name: {line}")
    return problems


# --- reading the log ---

LOG_LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})\] \[\w+\] \[[^\]]*\] \[[^\]]*\] (.*)$")
RE_MARK = re.compile(r"dev pad:\s+UI (.*)$")
RE_PADMARK = re.compile(r"dev pad: mark (\S+) at engine frame (\d+), swap (\d+)")
RE_ROW = re.compile(
    r"ui interp: (\S+) inst=([0-9A-F]{8}) d=(\d+) ser=(\d+) t=(\d+)\.(\d{3}) ms alpha=([\d.]+) "
    r"live=\(([-\d.]+),([-\d.]+)\) shown=\(([-\d.]+),([-\d.]+)\) world=\(([-\d.]+),([-\d.]+)\)"
    r"(?: lm=\(([-\d.,]+)\) sm=\(([-\d.,]+)\) cx=\((-?\d+),(-?\d+)\|(-?\d+),(-?\d+)\))? applied=(\d)")
RE_CHECK = re.compile(r"ui interp check: nodes=(\d+) matrix_off=(\d+) \(worst ([\d.]+)\) cxform_off=(\d+) gliding=(\d+) applied=(\d+)")
RE_STEP = re.compile(r"ui interp step: (\S+) ser=(\d+) moved=(\d+) cx=(\d+) fresh=(\d+) (?:swapped=(\d+) )?snapped=(\d+) gap=(\d+) "
                     r"max_move=([\d.]+)px tracked=(\d+)")
RE_MOVIE = re.compile(r"ui interp movie: (\S+) ctx=([0-9A-F]{8}) swf_fps=(\d+) period=(\d+) us")
RE_ADV = re.compile(r"ui adv: (\S+) ctx=([0-9A-F]{8}) t=(\d+)\.(\d{3}) ms in=(\d)")
RE_EVENT = re.compile(r"ui interp event: (\S+) kind=(swap|snap) (.*)$")
RE_COST = re.compile(r"ui interp cost: option=(on|off) calls=(\d+) total_us=(\d+) per_call_ns=(\d+) worst_us=([\d.]+) gliding=(\d+) applied=(\d+)")
RE_BURST = re.compile(r"dev pad: burst (\S+) k=(\d+) swap=(\d+)\.\.(\d+) t=(\d+)\.(\d{3}) ms size=(\d+)x(\d+)")

Row = namedtuple("Row", "screen inst depth ser t alpha live shown world lm sm cx applied")


def read_log(folder):
    """The run's log in time order. The game rotates its log at 5 MB (game.4.log is the oldest, game.1.log the newest rotated
    file, game.log the one being written), so a run that logs every glide spreads over all of them: reading game.log alone lost
    every cell before the last 1.6 MB (round 1: the cap-120 and opening cells)."""
    folder = Path(folder)
    parts = sorted(folder.glob("game.[0-9]*.log"), key=lambda p: -int(p.name.split(".")[1]))
    parts.append(folder / "game.log")
    return "".join(p.read_text(encoding="utf-8", errors="replace") for p in parts if p.exists())


def _kv(text):
    return dict(p.split("=", 1) for p in text.split() if "=" in p)


def _floats(text):
    return tuple(float(x) for x in text.split(",")) if text else None


def parse(text):
    """-> cells, checks, movies, marks. A cell: kind, variant, cap, id, scene, mark, t0, t1 (wall seconds of the day), phases {phase: [Row]},
    adv [(name, ms)], taps, census [dict], events [dict], cost [dict], bursts [dict]. marks: [(name, swap, frame, wall)] in order."""
    cells, checks, movies, marks = [], [], {}, []
    cur = None
    phase = "all"
    for line in text.splitlines():
        m = LOG_LINE.match(line)
        body = m.group(6) if m else line
        wall = int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4)) + int(m.group(5)) / 1000.0 if m else 0.0
        if cur is not None and "ui interp: " in body:  # the common line first (hundreds of thousands of them)
            mi = RE_ROW.search(body)
            if mi:
                g = mi.groups()
                cur["phases"].setdefault(phase, []).append(Row(
                    g[0], g[1], int(g[2]), int(g[3]), int(g[4]) + int(g[5]) / 1000.0, float(g[6]), (float(g[7]), float(g[8])),
                    (float(g[9]), float(g[10])), (float(g[11]), float(g[12])), _floats(g[13]), _floats(g[14]),
                    (int(g[15]), int(g[16]), int(g[17]), int(g[18])) if g[15] is not None else None, int(g[19])))
                continue
        pm = RE_PADMARK.search(body)
        if pm:
            marks.append((pm.group(1), int(pm.group(3)), int(pm.group(2)), wall))
            continue
        mm = RE_MARK.search(body)
        if mm:
            words = mm.group(1)
            if words.startswith("cell="):
                kv = _kv(words)
                cur = {"kind": kv.get("cell"), "variant": kv.get("variant"), "cap": int(kv.get("cap", 0)), "id": kv.get("id"),
                       "scene": kv.get("scene"), "mark": kv.get("mark"), "t0": wall, "t1": wall, "phases": {}, "adv": [],
                       "taps": [], "census": [], "events": [], "cost": [], "bursts": []}
                cells.append(cur)
                phase = "all"
            elif words.startswith("phase="):
                phase = words.split("=", 1)[1]
            elif words.startswith("tap "):
                if cur is not None:
                    cur["taps"].append(words)
            elif words.startswith("end"):
                if cur is not None:
                    cur["t1"] = wall
                cur = None
            continue
        mc = RE_CHECK.search(body)
        if mc:
            checks.append(tuple(int(x) if i != 2 else float(x) for i, x in enumerate(mc.groups())))
            continue
        mmv = RE_MOVIE.search(body)
        if mmv:
            movies[mmv.group(1)] = (int(mmv.group(3)), int(mmv.group(4)))
            continue
        if cur is None:
            continue
        mst = RE_STEP.search(body)
        if mst:
            g = mst.groups()
            cur["census"].append({"screen": g[0], "ser": int(g[1]), "moved": int(g[2]), "cx": int(g[3]), "fresh": int(g[4]),
                                  "swapped": int(g[5] or 0), "snapped": int(g[6]), "gap": int(g[7]), "max_move": float(g[8]),
                                  "tracked": int(g[9]), "wall": wall, "phase": phase})
            continue
        me = RE_EVENT.search(body)
        if me:
            ev = {"screen": me.group(1), "kind": me.group(2), "wall": wall, "phase": phase}
            ev.update(_kv(me.group(3)))
            cur["events"].append(ev)
            continue
        mco = RE_COST.search(body)
        if mco:
            g = mco.groups()
            cur["cost"].append({"option": g[0], "calls": int(g[1]), "total_us": int(g[2]), "per_call_ns": int(g[3]),
                                "worst_us": float(g[4]), "gliding": int(g[5]), "applied": int(g[6]), "wall": wall})
            continue
        mb = RE_BURST.search(body)
        if mb:
            g = mb.groups()
            cur["bursts"].append({"prefix": g[0], "k": int(g[1]), "swap0": int(g[2]), "swap1": int(g[3]),
                                  "t": int(g[4]) + int(g[5]) / 1000.0, "w": int(g[6]), "h": int(g[7])})
            continue
        ma = RE_ADV.search(body)
        if ma:
            cur["adv"].append((ma.group(1), int(ma.group(3)) + int(ma.group(4)) / 1000.0))
    return cells, checks, movies, marks


# --- what the rows say ---


def median(values, default=0):
    return statistics.median(values) if values else default


def rows_of(cell):
    return [r for ph in cell["phases"].values() for r in ph]


def drawn_key(r):
    """What was drawn: the world position, and the matrix and colour that were shown (applied) or the step's (not)."""
    m = (r.sm if r.applied else r.lm) or ()
    c = (r.cx[2], r.cx[3]) if (r.cx and r.applied) else ((r.cx[0], r.cx[1]) if r.cx else ())
    return (round(r.world[0], 2), round(r.world[1], 2), tuple(round(x, 3) for x in m), c)


def live_key(r):
    c = (r.cx[0], r.cx[1]) if r.cx else ()
    return (round(r.live[0], 2), round(r.live[1], 2), tuple(round(x, 3) for x in (r.lm or ())), c)


def screens_of(cell):
    kind = cell["kind"]
    if kind in ("hud", "hudrate", "hudburst"):
        return {"HUDGAUGE"}
    if kind in ("pause", "open", "close"):
        return {"PAUSEMOVIE"}
    return None  # every screen but the HUD's


def node_steps(rows, screens):
    """{(screen, inst): {ser: [Row]}} for the screens (None: the UI manager's screens other than the HUD's)."""
    by = defaultdict(lambda: defaultdict(list))
    for r in rows:
        if screens is None:
            if r.screen in HUD_SCREENS:
                continue
        elif r.screen not in screens:
            continue
        by[(r.screen, r.inst)][r.ser].append(r)
    return by


def min_draws(cap):
    return max(2, int(cap / 30 * 0.75)) if cap > 0 else 3


def smooth_stats(cell, screens=None):
    """Over the nodes that move: per (node, step) how many different things were drawn. -> dict."""
    screens = screens if screens is not None else screens_of(cell)
    by = node_steps(rows_of(cell), screens)
    vals, draws, nodes = [], [], 0
    need = min_draws(cell["cap"])
    for (screen, inst), steps in by.items():
        keys = {live_key(r) for rs in steps.values() for r in rs}
        if len(keys) < 4:
            continue  # not a mover
        nodes += 1
        for ser, rs in steps.items():
            if len(rs) >= need:
                vals.append(len({drawn_key(r) for r in rs}))
                draws.append(len(rs))
    return {"nodes": nodes, "samples": len(vals), "median": median(vals), "max": max(vals or [0]),
            "p10": sorted(vals)[len(vals) // 10] if vals else 0, "draws": median(draws)}


def top_movers(cell, n=3, screens=None):
    screens = screens if screens is not None else screens_of(cell)
    by = node_steps(rows_of(cell), screens)
    out = []
    for (screen, inst), steps in by.items():
        rs = sorted((r for rr in steps.values() for r in rr), key=lambda r: r.t)
        path = sum(abs(b.world[0] - a.world[0]) + abs(b.world[1] - a.world[1]) for a, b in zip(rs, rs[1:]) if b.t - a.t < 100)
        keys = len({live_key(r) for r in rs})
        out.append((path, keys, screen, inst, rs))
    out.sort(key=lambda x: (-x[0], -x[1]))
    return out[:n]


def anim_duration(cell, screens=None, min_travel=150.0):
    """Per node that travels far in this cell: the time its drawn position takes from 10% to 90% of the way (ms); -> (median, nodes) or None."""
    screens = screens if screens is not None else screens_of(cell)
    by = node_steps(rows_of(cell), screens)
    durs = []
    for (screen, inst), steps in by.items():
        rs = sorted((r for rr in steps.values() for r in rr), key=lambda r: r.t)
        if len(rs) < 4:
            continue
        w0, wl = rs[0].world, rs[-1].live
        total = ((wl[0] - w0[0]) ** 2 + (wl[1] - w0[1]) ** 2) ** 0.5
        if total < min_travel:
            continue
        t_start = next((r.t for r in rs if ((r.world[0] - w0[0]) ** 2 + (r.world[1] - w0[1]) ** 2) ** 0.5 >= 0.1 * total), None)
        t_end = next((r.t for r in rs if ((r.world[0] - wl[0]) ** 2 + (r.world[1] - wl[1]) ** 2) ** 0.5 <= 0.1 * total), None)
        if t_start is not None and t_end is not None and t_end > t_start:
            durs.append(t_end - t_start)
    return (median(durs), len(durs)) if durs else None


def step_duration(cell, screen="PAUSEMOVIE", min_move=2.0):
    """The wall time from the first to the last step of the screen that moved something by min_move px or more (ms), and how many steps."""
    cs = [c for c in cell["census"] if c["screen"] == screen and c["max_move"] >= min_move]
    if len(cs) < 2:
        return None
    return (cs[-1]["wall"] - cs[0]["wall"]) * 1000.0, len(cs)


def census_line(cell, screen):
    cs = [c for c in cell["census"] if c["screen"] == screen]
    if not cs:
        return None
    moving = [c for c in cs if c["moved"] or c["cx"] or c["fresh"] or c["swapped"]]
    span = max(cell["t1"] - cell["t0"], 1e-3)
    return {"steps": len(cs), "per_s": len(cs) / span, "with_changes": len(moving),
            "moved": median([c["moved"] for c in moving]), "moved_max": max([c["moved"] for c in moving] or [0]),
            "cx": median([c["cx"] for c in moving]), "fresh": sum(c["fresh"] for c in cs), "swapped": sum(c["swapped"] for c in cs),
            "snapped": sum(c["snapped"] for c in cs), "gap": sum(c["gap"] for c in cs),
            "largest": max([c["max_move"] for c in cs] or [0]), "tracked": median([c["tracked"] for c in cs])}


def events_summary(cells):
    """What swapped and what snapped, over the cells: {(cell kind, screen, event): [count, nodes, example]}."""
    out = {}
    for cell in cells:
        for ev in cell["events"]:
            what = ev["kind"] if ev["kind"] == "swap" else f"snap-{ev.get('why', '?')}"
            e = out.setdefault((cell["kind"], ev["screen"], what), [0, set(), None])
            e[0] += 1
            e[1].add(ev.get("inst"))
            if e[2] is None:
                e[2] = ev
    return out


def describe_cell(cell, lines):
    rows = rows_of(cell)
    head = f"== {cell['kind']} variant {cell['variant']} cap {cell['cap']}" + (f" id {cell['id']}" if cell["id"] else "") + \
           (f" scene {cell['scene']}" if cell["scene"] else "")
    lines.append(head)
    if cell["kind"] == "cost":
        return
    screens = {c["screen"] for c in cell["census"]} | {r.screen for r in rows}
    if not rows:
        lines.append("   no gliding element logged (nothing moved, or the hook did not run)")
    else:
        applied = sum(r.applied for r in rows)
        lines.append(f"   {len(rows)} logged draws of gliding elements, {applied} of them applied (applied=1)")
        st = smooth_stats(cell)
        lines.append(f"   movers on {sorted(screens_of(cell) or ['the UI screens'])}: {st['nodes']} nodes; different things drawn per step: "
                     f"median {st['median']:.0f}, p10 {st['p10']}, max {st['max']} over {st['samples']} node-steps "
                     f"(draws per step median {st['draws']:.0f})")
        for path, keys, screen, inst, rs in top_movers(cell, 2):
            lines.append(f"   mover {screen} {inst} (depth {rs[0].depth}): path {path:.1f} px over {len(rs)} draws, {keys} different step values")
    for screen in sorted(screens):
        c = census_line(cell, screen)
        if c:
            lines.append(f"   census {screen}: {c['steps']} steps ({c['per_s']:.1f}/s), {c['with_changes']} with changes; per step with changes: "
                         f"{c['moved']:.0f} nodes moved (max {c['moved_max']}), {c['cx']:.0f} colour transforms; in all: {c['fresh']} new, "
                         f"{c['swapped']} another character, {c['snapped']} snapped, {c['gap']} missed steps; largest move {c['largest']:.1f} px; "
                         f"{c['tracked']:.0f} nodes tracked")


# --- pictures ---


def _pil():
    try:
        from PIL import Image, ImageChops, ImageFilter  # noqa: F401
        return True
    except ImportError:
        return False


def _changed(a, b, threshold=6):
    from PIL import ImageChops
    d = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > threshold else 0)
    return d.histogram()[255], d


def rest_check(shots, lines, verdict):
    """Pictures at rest: the difference between the option off and on must lie where the picture changes by itself (two
    pictures of the same variant, taken a moment apart, differ there too)."""
    from PIL import Image, ImageChops, ImageFilter
    shots = Path(shots)
    for cap in (120, 60):
        pics = {v: [shots / f"rest-{v}-{cap}-{k}.png" for k in range(3)] for v in OFF_ON}
        if not all(p.exists() for ps in pics.values() for p in ps):
            continue
        im = {v: [Image.open(p).convert("RGB") for p in ps] for v, ps in pics.items()}
        selfmask = None
        for v in OFF_ON:
            for a, b in zip(im[v], im[v][1:]):
                _, d = _changed(a, b)
                selfmask = d if selfmask is None else ImageChops.lighter(selfmask, d)
        grown = selfmask.filter(ImageFilter.MaxFilter(9))
        cross = []
        for a, b in zip(im["off"], im["on"]):
            n, d = _changed(a, b)
            cross.append((n, d.getbbox(), ImageChops.subtract(d, grown).histogram()[255]))
        outside = sum(c[2] for c in cross)
        lines.append(f"rest pictures at cap {cap}: pixels that change by themselves (two pictures of one variant) {selfmask.histogram()[255]}; "
                     f"off against on: " + ", ".join(f"{n} px in {box}" for n, box, _ in cross) + f"; outside the self-changing area: {outside} px")
        verdict.append(f"{'PASS' if outside == 0 else 'CHECK'} rest {cap}: the pictures at rest with the option off and on differ "
                       f"{'only where the picture changes by itself (an animated icon)' if outside == 0 else 'also elsewhere: ' + str(outside) + ' px'}")


def burst_check(shots, cells, lines, verdict, label):
    """Bursts of consecutive pictures: how many of them differ from the one before (a staircase changes once per step, a glide on every frame)."""
    from PIL import Image
    shots = Path(shots)
    sheet_rows, summary = [], {}
    prefixes = sorted({p.name.rsplit("-", 1)[0] for p in shots.glob("*-*-*-*.png") if p.name.startswith(("burst-", "hud-"))})
    for prefix in prefixes:
        files = sorted(shots.glob(f"{prefix}-*.png"), key=lambda p: int(p.stem.rsplit("-", 1)[1]))
        if len(files) < 3:
            continue
        imgs = [Image.open(f).convert("RGB") for f in files]
        counts = [_changed(a, b)[0] for a, b in zip(imgs, imgs[1:])]
        bursts = [b for c in cells for b in c["bursts"] if Path(b["prefix"]).name == prefix]
        swaps = [(b["swap0"], b["swap1"]) for b in sorted(bursts, key=lambda b: b["k"])]
        skipped = sum(max(0, s0 - s1p - 1) for (_, s1p), (s0, _) in zip(swaps, swaps[1:])) if swaps else None
        summary[prefix] = (counts, skipped, swaps)
        sheet_rows.append((prefix, imgs))
    for prefix, (counts, skipped, swaps) in sorted(summary.items()):
        moving = sum(1 for c in counts if c > 0)
        lines.append(f"burst {label}{prefix}: {len(counts) + 1} pictures, swaps {swaps[0][0] if swaps else '?'}..{swaps[-1][1] if swaps else '?'}"
                     f" ({skipped if skipped is not None else '?'} swaps skipped by the capture); changed pixels from one picture to the next: {counts}; "
                     f"{moving} of {len(counts)} pairs differ")
    for kind in sorted({p.rsplit("-", 2)[0] for p in summary}):
        for cap in sorted({p.rsplit("-", 1)[1] for p in summary if p.startswith(kind + "-")}):
            off, on = summary.get(f"{kind}-off-{cap}"), summary.get(f"{kind}-on-{cap}")
            if off and on:
                m_off, m_on = sum(1 for c in off[0] if c > 0), sum(1 for c in on[0] if c > 0)
                verdict.append(f"{'PASS' if m_on > m_off else 'CHECK'} burst {label}{kind} cap {cap}: consecutive pictures that differ: off {m_off} of {len(off[0])}, "
                               f"on {m_on} of {len(on[0])} ({(off[1] or 0) + (on[1] or 0)} swaps skipped by the capture)")
    return sheet_rows


def contact_sheet(sheet_rows, path, tile_w=200):
    from PIL import Image, ImageDraw
    if not sheet_rows:
        return
    cols = max(len(imgs) for _, imgs in sheet_rows)
    tiles = []
    for name, imgs in sheet_rows:
        tiles.append((name, [im.resize((tile_w, max(1, int(im.height * tile_w / im.width))), Image.LANCZOS) for im in imgs]))
    tile_h = max(t.height for _, row in tiles for t in row)
    sheet = Image.new("RGB", (cols * tile_w, len(tiles) * (tile_h + 14)), (255, 255, 255))
    d = ImageDraw.Draw(sheet)
    for r, (name, row) in enumerate(tiles):
        d.text((2, r * (tile_h + 14)), name, fill=(0, 0, 0))
        for c, t in enumerate(row):
            sheet.paste(t, (c * tile_w, r * (tile_h + 14) + 12))
    sheet.save(path)


# --- frame times and CPU ---


def frame_segments(folder):
    """frames.csv -> {mark name: [frame ms]} (a segment runs from its mark to the next; repeated names keep the last)."""
    path = Path(folder) / "frames.csv"
    if not path.exists():
        return {}
    segments = [("start", [])]
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        kind, *rest = line.split(",")
        if len(rest) < 3 or not all(f.strip("-").isdigit() for f in rest[1:3]):
            continue
        if kind == "mark":
            segments.append((rest[0], []))
        elif kind == "s" and int(rest[1]) > 0:
            segments[-1][1].append(int(rest[1]) / 1000.0)
    return {name: ms for name, ms in segments}


def frame_stats(ms):
    if len(ms) < 5:
        return None
    o = sorted(ms)
    return {"frames": len(ms), "seconds": sum(ms) / 1000.0, "median": statistics.median(ms), "p99": o[min(len(o) - 1, int(len(o) * 0.99))],
            "fps": 1000.0 * len(ms) / sum(ms)}


def thread_cpu(folder, swap0, swap1):
    """Process CPU per frame between two swap counts, from the 1 s rows of --mcla_profile_thread_log (rows fully inside)."""
    path = Path(folder) / "threads.csv"
    if not path.exists():
        return None
    by = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        f = line.split(",")
        if len(f) < 7 or not f[0].lstrip("-").isdigit():
            continue
        e = by.setdefault(int(f[0]), {"swaps": int(f[1]), "cpu": 0.0, "threads": defaultdict(float)})
        e["cpu"] += float(f[5])
        e["threads"][f[4]] += float(f[5])
    ts = sorted(by)
    cpu, frames, thr = 0.0, 0, defaultdict(float)
    for a, b in zip(ts, ts[1:]):
        if by[a]["swaps"] >= swap0 and by[b]["swaps"] <= swap1:
            cpu += by[b]["cpu"]
            frames += by[b]["swaps"] - by[a]["swaps"]
            for k, v in by[b]["threads"].items():
                thr[k] += v
    if frames < 50:
        return None
    return {"ms_per_frame": cpu / frames, "frames": frames, "threads": {k: v / frames for k, v in sorted(thr.items(), key=lambda kv: -kv[1])[:3]}}


def cost_report(folder, cells, marks, lines, verdict, label):
    segs = frame_segments(folder)
    table = defaultdict(list)  # (scene, cap, variant) -> [result dicts]
    for cell in cells:
        if cell["kind"] != "cost":
            continue
        fs = frame_stats(segs.get(cell["mark"], [])) if cell["mark"] else None
        costs = [c for c in cell["cost"] if c["calls"] > 20]  # the hook's own time (mcla_ui_interp_stats), one line a second
        hook_us_s = median([c["total_us"] for c in costs]) if costs else None
        per_call = median([c["per_call_ns"] for c in costs]) if costs else None
        worst = max([c["worst_us"] for c in costs] or [0])
        calls_s = median([c["calls"] for c in costs]) if costs else None
        cpu = None  # the process's CPU per frame, from this mark to the next
        idx = next((i for i, m in enumerate(marks) if m[0] == cell["mark"]), None)
        if idx is not None and idx + 1 < len(marks):
            cpu = thread_cpu(folder, marks[idx][1], marks[idx + 1][1])
        r = {"fs": fs, "hook_us_s": hook_us_s, "per_call": per_call, "worst": worst, "calls_s": calls_s, "cpu": cpu,
             "glide": median([c["gliding"] for c in costs]) if costs else None, "applied": median([c["applied"] for c in costs]) if costs else None}
        table[(cell["scene"], cell["cap"], cell["variant"])].append(r)
        fps = fs["fps"] if fs else None
        lines.append(f"cost {label}{cell['scene']} cap {cell['cap']} {cell['variant']:3s} #{cell['id']}: "
                     + (f"frames {fs['frames']} median {fs['median']:.2f} ms p99 {fs['p99']:.2f} ms ({fs['fps']:.0f}/s); " if fs else "no frame times; ")
                     + (f"hook {hook_us_s:.0f} us/s = {hook_us_s / fps:.1f} us/frame, {per_call:.0f} ns/call, {calls_s:.0f} calls/s, worst call {worst:.1f} us, "
                        f"gliding {r['glide']:.0f}/s applied {r['applied']:.0f}/s; " if (hook_us_s is not None and fps) else "no hook cost lines; ")
                     + (f"process CPU {cpu['ms_per_frame']:.3f} ms/frame over {cpu['frames']} frames" if cpu else "no thread CPU"))
    for scene in sorted({k[0] for k in table}):
        for cap in sorted({k[1] for k in table if k[0] == scene}):
            off, on = table.get((scene, cap, "off"), []), table.get((scene, cap, "on"), [])
            if not off or not on:
                continue

            def avg(group, f):
                vals = [f(r) for r in group if f(r) is not None]
                return sum(vals) / len(vals) if vals else None

            hook_f = lambda r: r["hook_us_s"] / r["fs"]["fps"] if (r["hook_us_s"] is not None and r["fs"]) else None  # noqa: E731
            parts = []
            for what, f, unit in (("frame median", lambda r: r["fs"]["median"] if r["fs"] else None, "ms"),
                                  ("frame p99", lambda r: r["fs"]["p99"] if r["fs"] else None, "ms"),
                                  ("process CPU", lambda r: r["cpu"]["ms_per_frame"] if r["cpu"] else None, "ms/frame"),
                                  ("hook", hook_f, "us/frame")):
                a, b = avg(off, f), avg(on, f)
                if a is not None and b is not None:
                    parts.append(f"{what} {a:.3f} -> {b:.3f} {unit}")
                elif b is not None:
                    parts.append(f"{what} on {b:.3f} {unit}")
            lines.append(f"cost {label}{scene} cap {cap} (mean of {len(off)} off, {len(on)} on): " + "; ".join(parts))
            hook, med = avg(on, hook_f), avg(on, lambda r: r["fs"]["median"] if r["fs"] else None)
            if hook is not None and med:
                verdict.append(f"{'PASS' if hook < 30 * med else 'CHECK'} cost {label}{scene} cap {cap}: the hook takes {hook:.0f} us per frame with the option on "
                               f"({hook / 10 / med:.1f}% of the {med:.2f} ms frame; limit 3%)")


# --- the report ---


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
        ys_ = [r.world[1] for r in rs]
        xs_ = [r.world[0] for r in rs]
        use_y = (max(ys_) - min(ys_)) >= (max(xs_) - min(xs_))
        vals = ys_ if use_y else xs_
        lo, hi = min(vals), max(vals)
        t0, t1 = rs[0].t, rs[-1].t
        span = max(t1 - t0, 1e-6)
        rng = max(hi - lo, 1e-6)
        pts = [(M + (r.t - t0) / span * (W - 2 * M), y0 + H - M - ((v - lo) / rng) * (H - 2 * M)) for r, v in zip(rs, vals)]
        d.line([(M, y0 + H - M), (W - M, y0 + H - M)], fill=(160, 160, 160))
        for a, b in zip(pts, pts[1:]):
            d.line([a, b], fill=(120, 160, 230))
        for p in pts:
            d.ellipse([p[0] - 2, p[1] - 2, p[0] + 2, p[1] + 2], fill=(200, 40, 40))
        d.text((M, y0 + H - M + 6),
               f"{label}: {len(rs)} draws, {'y' if use_y else 'x'} {lo:.1f}..{hi:.1f} px over {span:.0f} ms (one dot = one drawn frame)", fill=(0, 0, 0))
    img.save(path)


def ascii_steps(rs, cap, n=14):
    """The first step with the full number of draws: a line per draw."""
    need = min_draws(cap)
    steps = defaultdict(list)
    for r in rs:
        steps[r.ser].append(r)
    for ser, items in sorted(steps.items()):
        if len(items) >= need + 1 and len({drawn_key(i) for i in items}) >= 2:
            t0 = items[0].t
            return [f"      step {ser}: " + ", ".join(f"+{i.t - t0:.1f}ms ({i.world[0]:.1f},{i.world[1]:.1f})" for i in items[:n])]
    return []


def compare_cells(cells, kind, cap, verdict):
    """Smoothness off against on for one kind of cell at one cap: the repeats are pooled."""
    res = {}
    for v in OFF_ON:
        cs = [c for c in cells if c["kind"] == kind and c["cap"] == cap and c["variant"] == v]
        if not cs:
            return None
        stats = [smooth_stats(c) for c in cs]
        res[v] = (median([s["median"] for s in stats]), max(s["max"] for s in stats), sum(s["samples"] for s in stats),
                  sum(s["nodes"] for s in stats))
    want = 1.9 if cap == 60 else 3.0
    ok = res["on"][0] >= want and res["off"][0] <= 1.5 and res["on"][2] > 0
    verdict.append(f"{'PASS' if ok else 'CHECK'} smooth {kind} cap {cap}: different things drawn per 30 Hz step, median over the moving nodes: "
                   f"off {res['off'][0]:.0f} (max {res['off'][1]}, {res['off'][3]} nodes), on {res['on'][0]:.0f} (max {res['on'][1]}, {res['on'][3]} nodes); "
                   f"{cap / 30:.0f} draws a step")
    return res


def report_run(folder, tag, lines, verdict):
    folder = Path(folder)
    text = read_log(folder)
    cells, checks, movies, marks = parse(text)
    label = "" if tag == "trace" else f"[{tag}] "
    lines.append(f"##### run {tag}: {folder}  ({len(cells)} cells, {sum(len(rows_of(c)) for c in cells)} rows, {len(checks)} self-check lines, "
                 f"{len(text) // 1000000} MB of log)")
    if movies and tag == "trace":
        lines.append("movies seen drawn (the SWF's own frame rate, the pace they were stepped at): " + ", ".join(
            f"{n} swf {fps / 256:.1f}/s stepped every {per / 1000:.1f} ms" for n, (fps, per) in sorted(movies.items())))
    for cell in cells:
        describe_cell(cell, lines)
        if cell["kind"] in ("pause", "hud") and cell["variant"] == "on":
            mv = top_movers(cell, 1)
            if mv:
                lines += ascii_steps(mv[0][4], cell["cap"])
    if checks:
        last = checks[-1]
        ok = last[1] == 0 and last[3] == 0
        verdict.append(f"{'PASS' if ok else 'FAIL'} arithmetic {label}: {last[0]} nodes checked, matrix differs from the game's world matrix in {last[1]} "
                       f"(worst {last[2]:.5f}), colour transform in {last[3]}")
    elif tag == "trace":
        verdict.append("FAIL arithmetic: no `ui interp check` line in the log (the hook did not run, or logging was not on)")
    return cells, marks


def report_trace(cells, shots, lines, verdict):
    for kind in ("pause", "hud", "title"):
        for cap in (120, 60):
            compare_cells(cells, kind, cap, verdict)
    offs = [census_line(c, "HUDGAUGE") for c in cells if c["kind"] == "hudrate" and c["variant"] == "off"]
    ons = [census_line(c, "HUDGAUGE") for c in cells if c["kind"] == "hudrate" and c["variant"] == "on"]
    if offs and ons and offs[0] and ons[0]:
        ok = 26 <= ons[0]["per_s"] <= 34 and offs[0]["per_s"] >= 45
        verdict.append(f"{'PASS' if ok else 'CHECK'} HUD steps: with mcla_ui_hud_rate 60 the HUD stepped {offs[0]['per_s']:.0f}/s with the option off "
                       f"and {ons[0]['per_s']:.0f}/s with it on (the option pins every movie to 30/s)")
    else:
        verdict.append("CHECK HUD steps: the hudrate cells are missing")
    for cap, want in ((60, True), (30, False)):
        cs = [c for c in cells if c["kind"] == "pause" and c["cap"] == cap and c["variant"] == "on"]
        if not cs:
            verdict.append(f"CHECK cap {cap}: cell missing")
            continue
        rows = [r for c in cs for r in rows_of(c)]
        applied = sum(r.applied for r in rows)
        ok = (applied > 0) if want else (applied == 0)
        verdict.append(f"{'PASS' if ok else 'FAIL'} cap {cap}: {applied} of {len(rows)} gliding draws applied (expected {'some' if want else 'none'})")
    for what in ("open", "close"):
        res = {}
        for v in OFF_ON:
            cs = [c for c in cells if c["kind"] == what and c["variant"] == v and c["cap"] == 120]
            drawn = [anim_duration(c) for c in cs]
            steps = [step_duration(c) for c in cs]
            res[v] = ([d[0] for d in drawn if d], [s[0] for s in steps if s], [s[1] for s in steps if s], [d[1] for d in drawn if d])
        if res["off"][0] and res["on"][0]:
            d_off, d_on = median(res["off"][0]), median(res["on"][0])
            s_off, s_on = median(res["off"][1]), median(res["on"][1])
            ok = abs(d_on - d_off) <= 40 and abs(s_on - s_off) <= 40
            lines.append(f"{what}: 10%-90% of the travel of {median(res['off'][3]):.0f} nodes: {d_off:.0f} ms off, {d_on:.0f} ms on; "
                         f"first to last moving step {s_off:.0f} ms off, {s_on:.0f} ms on ({median(res['off'][2]):.0f} and {median(res['on'][2]):.0f} steps)")
            verdict.append(f"{'PASS' if ok else 'CHECK'} duration {what}: the animation takes {d_off:.0f} ms (drawn, option off) and {d_on:.0f} ms (option on); "
                           f"by steps {s_off:.0f} and {s_on:.0f} ms (equal within one 33 ms step: {'yes' if ok else 'NO'})")
        else:
            verdict.append(f"CHECK duration {what}: no node that travels far in both variants (off {len(res['off'][0])} cells, on {len(res['on'][0])})")
    ev = events_summary(cells)
    lines.append("")
    lines.append("=== events: nodes that changed character (swap) or snapped, per cell kind")
    for (kind, screen, what), (n, nodes, ex) in sorted(ev.items()):
        ex_text = " ".join(f"{k}={v}" for k, v in (ex or {}).items() if k not in ("wall", "phase", "screen", "kind"))
        lines.append(f"   {kind:9s} {screen:12s} {what:12s} {n:5d} events, {len(nodes)} nodes; e.g. {ex_text}")
    count = lambda w: sum(n for (k, s, x), (n, _, _) in ev.items() if x == w)  # noqa: E731
    verdict.append(f"NOTE snaps and swaps: {count('snap-turned')} snap events because a node turned round and jumped (the HUD ticker's wrap), "
                   f"{count('snap-far')} because it moved farther than mcla_ui_interp_snap_px, {count('snap-flip')} flips; {count('swap')} character swaps logged")
    lines.append("")
    lines.append("=== census per cell kind and screen (option on, cap 120): what glides and what cannot")
    agg = defaultdict(lambda: defaultdict(float))
    for c in cells:
        if c["variant"] != "on" or c["cap"] != 120 or c["kind"] == "cost":
            continue
        for screen in {x["screen"] for x in c["census"]}:
            cl = census_line(c, screen)
            if cl:
                a = agg[(c["kind"], screen)]
                for k in ("steps", "with_changes", "fresh", "swapped", "snapped"):
                    a[k] += cl[k]
                a["moved"] = max(a["moved"], cl["moved_max"])
                a["tracked"] = max(a["tracked"], cl["tracked"])
                a["largest"] = max(a["largest"], cl["largest"])
    lines.append(f"   {'cell':9s} {'screen':12s} {'steps':>6s} {'changing':>9s} {'max moved/step':>14s} {'new':>5s} {'swapped':>8s} {'snapped':>8s} {'largest px':>10s} {'tracked':>8s}")
    for (kind, screen), a in sorted(agg.items()):
        lines.append(f"   {kind:9s} {screen:12s} {a['steps']:6.0f} {a['with_changes']:9.0f} {a['moved']:14.0f} {a['fresh']:5.0f} "
                     f"{a['swapped']:8.0f} {a['snapped']:8.0f} {a['largest']:10.1f} {a['tracked']:8.0f}")


def report(folder, out=None, shots=None, plot_path=None, echo=True):
    """<folder>: a folder with the runs trace\\, cost\\, big\\ (or, as round 1 left it, game.log itself: one run)."""
    folder = Path(folder)
    lines, verdict = [f"ui_interp report of {folder}"], []
    runs = [(t, folder / t) for t in ("trace", "cost", "big") if (folder / t / "game.log").exists()]
    if not runs and (folder / "game.log").exists():
        runs = [("trace", folder)]
    done = {}
    for tag, path in runs:
        cells, marks = report_run(path, tag, lines, verdict)
        done[tag] = (cells, marks, path)
    if "trace" in done:
        cells = done["trace"][0]
        lines.append("")
        report_trace(cells, shots, lines, verdict)
        pics = Path(shots) / "trace" if shots and (Path(shots) / "trace").exists() else (Path(shots) if shots else None)
        if pics and _pil():
            lines.append("")
            lines.append("=== pictures")
            try:
                rest_check(pics, lines, verdict)
            except Exception as e:  # noqa: BLE001
                verdict.append(f"CHECK rest: could not compare the pictures ({e})")
            try:
                rows = burst_check(pics, cells, lines, verdict, "")
                contact_sheet(rows, Path(out).with_name("interp-bursts.png") if out else pics / "bursts.png")
            except Exception as e:  # noqa: BLE001
                verdict.append(f"CHECK burst: could not read the pictures ({e})")
        elif pics:
            lines.append("pictures: not compared (Pillow is missing: run the report with the scratchpad's upstream\\bench-venv python)")
        if plot_path and _pil():
            ser = []
            for v in OFF_ON:
                cs = [c for c in cells if c["kind"] == "pause" and c["cap"] == 120 and c["variant"] == v]
                mv = top_movers(cs[0], 1, {"PAUSEMOVIE"}) if cs else []
                if mv:
                    ser.append((f"pause menu, option {v}, cap 120, {mv[0][3]}", [r for r in mv[0][4] if r.t - mv[0][4][0].t < 1500][:200]))
            if ser:
                plot(ser, plot_path, "world position")
                lines.append(f"plot: {plot_path}")
    for tag in ("cost", "big"):
        if tag in done:
            cells, marks, path = done[tag]
            lines.append("")
            lines.append(f"=== cost, run {tag}")
            cost_report(path, cells, marks, lines, verdict, "" if tag == "cost" else "[3x] ")
            big_pics = Path(shots) / "big" if shots else None
            if tag == "big" and big_pics and big_pics.exists() and _pil():
                try:
                    rows = burst_check(big_pics, cells, lines, verdict, "[3x] ")
                    contact_sheet(rows, Path(out).with_name("interp-bursts-3x.png") if out else big_pics / "bursts.png", 260)
                except Exception as e:  # noqa: BLE001
                    verdict.append(f"CHECK burst 3x: {e}")
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
    p.add_argument("kind", choices=["trace", "cost", "big"])
    p.add_argument("out")
    p.add_argument("--shots", default="{SHOT}")
    r = sub.add_parser("report")
    r.add_argument("folder")
    r.add_argument("--out")
    r.add_argument("--shots")
    r.add_argument("--plot")
    a = ap.parse_args()
    if a.cmd == "pad":
        lines = pad_lines(a.kind, a.shots)
        problems = lint(lines)
        if problems:
            print("\n".join(problems))
            return 1
        Path(a.out).write_text("\n".join(lines) + "\n", encoding="ascii")
        print(f"{a.out}: {len(lines)} lines")
        return 0
    report(a.folder, a.out, a.shots, a.plot)
    return 0


if __name__ == "__main__":
    sys.exit(main())

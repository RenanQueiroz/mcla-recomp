"""Menu input check: how many steps one press makes (fork-only development tool).

  menu_input.py pad <out.pad> --set res1|res3|fg|merged|lag   write the dev pad script of one run
  menu_input.py report <run folder>... [--out FILE]    read the runs' game.log and say what each press did
  menu_input.py lagreport <run folder>... [--out FILE] how soon the menu's movie advanced after each key event
                                                       (the run of --set lag; needs --mcla_ui_advance_log=true)

The script (made by `pad`) takes the game to the pause menu's Settings tab, opens PERFORMANCE and, for
each cell of its list (a frame-rate cap, mcla_ui_timing_fix on or off, mcla_ui_repeat_fix on or off,
all changed while the game runs), makes the presses of a player:

  T0   the pause menu's own list: 4 taps of "down", 4 of "up" (press lengths 60 to 150 ms, 450 ms apart)
  T1   PERFORMANCE: 8 taps of "down", 8 of "up"
  T2   PERFORMANCE: "down" held for 1 s
  T3   PERFORMANCE, CITY DETAIL DISTANCE (three values): 6 taps of "right", 3 of "left", and the option's
       value read after each
  T4   PERFORMANCE, GPU WAIT YIELD (a checkbox): 4 presses of A, the option's value read after each
  T5   DISPLAY, PEAK BRIGHTNESS (the number row with a hold of its own, HoldSteps in src/settings_menu.cpp):
       4 taps of "right", then "right" held for 3 s and "left" for 3 s; the option's value read after each
       (variants A and B only)

and logs a line before each press (`dev pad: MI tap ...`). The game logs every menu key down and
repeat (`ui key: ...`, mcla_ui_input_log) and the dev pad's `uisel` the list's selected row. `report`
counts the key events of each press: a tap should make exactly 1, and a hold of 1 s at the console's
pace (a repeat after 233 ms, then every 200 ms) 5.

Run it with devrun.py (a scratch save and settings, never the player's):
  devrun.py run --script <out.pad> --fps 60 --res 1 --extra --mcla_ui_input_log=true
"""
import argparse
import re
import sys
from pathlib import Path

TAP_LENS = [60, 100, 150, 80, 120, 60, 100, 150]
CAPS = [120, 0, 60, 30]  # the caps that matter first: a run cut short still has them
# (timing fix, repeat fix): A = as shipped in v0.3.0, B = the repeat fix, C = the timing fix off (the
# workaround), D = both off the shipped way round (timing fix off, repeat fix on).
VARIANTS = {"A": (1, 0), "B": (1, 1), "C": (0, 0), "D": (0, 1)}
# S = the build's defaults: the pad changes neither option (their values are read and logged), so it is
# what a player gets; the log line says timing 1, repeat 1, which are the defaults since the repeat fix
# became the default. O = the repeat fix off by `set` (the control: it fails above 30 FPS).
VARIANTS.update({"S": (1, 1), "O": (1, 0)})
# The set "lag" (mcla_ui_advance_on_input, 2026-10-29): the timing and repeat fixes are on in every cell.
# (advance on input, menu rate): A = the build before (next slot), B = advance on input, R = B with the menus
# at 60 a second, Z = B with the menus at every frame.
LAG_VARIANTS = {"A": (0, 30), "B": (1, 30), "R": (1, 60), "Z": (1, 0)}
LAG_CELLS = [(120, "A"), (120, "B"), (0, "A"), (0, "B"), (60, "A"), (60, "B"), (120, "R"), (120, "Z")]
LAG_OPEN = "ABAB"  # the pause menu's opening time, at the end: variant of each opening

KEY_NAMES = {47: "dpad up", 48: "dpad down", 51: "dpad left", 52: "dpad right", 49: "stick up",
             50: "stick down", 53: "stick left", 54: "stick right", 55: "A", 56: "B"}


NAV_CAP = 60  # the cap while the pad moves about: a 40 ms press stays under the game's 7 updates (117 ms) at it


def pad_lines(run_set):
    out = []
    add = out.append

    def tap(test, key, lens, extra=None):
        for i, length in enumerate(lens):
            add(f"log MI tap {test} i={i} len={length}")
            add(f"press {key} {length}ms")
            if extra:
                add("wait 300ms")
                add(extra)
                add("wait 150ms")
            else:
                add("wait 450ms")
            add("log MI tapend")

    def reset(to=0):
        add(f"press_until up ui_select == {to} every=500ms hold=50ms timeout=40s")
        add("wait 400ms")

    def goto(row):
        add(f"press_until down ui_select == {row} every=600ms hold=40ms timeout=30s")
        add("wait 400ms")

    add("# Made by menu_input.py (see its docstring). Do not edit: change the generator.")
    add("wait 8s")
    add("mark title")
    add("press_until start left_title every=2s timeout=90s")
    add("mark map_zoom")
    add("wait_until roam 180s")
    add("wait 4s")
    add("mark roam")
    # Short presses for the set-up: no repeat at any frame rate, whatever the options are.
    add("press start 100ms")
    add("wait 4500ms")
    add("wait_until ui_count >= 0 20s")
    add("press rb 40ms")
    add("wait 1200ms")
    add("press rb 40ms")
    add("wait 1500ms")
    add("log MI setup the Settings tab")
    add("uisel")

    if run_set == "res1":
        cells = [(c, v) for c in CAPS for v in ("ABCD" if c in (120, 0) else "AB")]
        fg = 0
    elif run_set == "res3":
        cells = [(120, v) for v in "ABCD"]
        fg = 0
    elif run_set == "fg":
        cells = [(60, v) for v in "AB"]
        fg = 1
    elif run_set == "merged":
        # the defaults at the caps that matter most, then the control (about 7 minutes in all)
        cells = [(120, "S"), (0, "S"), (120, "O")]
        fg = 0
    elif run_set == "lag":
        cells = LAG_CELLS
        fg = 0
    else:
        raise SystemExit(f"unknown set {run_set}")
    if fg:
        add("set frame_generation fsr")
        add("get frame_generation fg_status mcla_fps mcla_fps_frame_generation")

    lag = run_set == "lag"
    for n, (cap, variant) in enumerate(cells):
        timing, repeat = (1, 1) if lag else VARIANTS[variant]
        adv, rate = LAG_VARIANTS[variant] if lag else (None, None)

        def at_cap():
            add(f"set mcla_fps {cap}")
            if fg:
                add(f"set mcla_fps_frame_generation {cap}")
            add("wait 800ms")

        def at_nav():
            add(f"set mcla_fps {NAV_CAP}")
            if fg:
                add(f"set mcla_fps_frame_generation {NAV_CAP}")
            add("wait 800ms")

        add(f"# --- cell {n}: cap {cap}, timing fix {timing}, repeat fix {repeat}, frame generation {fg}")
        if variant == "S":
            add("get mcla_ui_timing_fix mcla_ui_repeat_fix")
        else:
            add(f"set mcla_ui_timing_fix {'true' if timing else 'false'}")
            add(f"set mcla_ui_repeat_fix {'true' if repeat else 'false'}")
        if lag:
            add(f"set mcla_ui_advance_on_input {'true' if adv else 'false'}")
            add(f"set mcla_ui_menu_rate {rate}")
        at_nav()
        add(f"log MI cell id={n} variant={variant} cap={cap} timing={timing} repeat={repeat} fg={fg}"
            + (f" adv={adv} rate={rate}" if lag else ""))
        reset(0)
        # T0: the pause menu's own list
        at_cap()
        if lag:
            # nothing pressed: how often the menu's movies advance by themselves
            add("log MI test IDLE key=none expect=0")
            add("wait 3000ms")
            add("log MI end IDLE")
        add("log MI test T0 key=down n=4 expect=4")
        add("uisel")
        tap("T0", "down", TAP_LENS[:4])
        add("wait 300ms")
        add("uisel")
        add("log MI test T0b key=up n=4 expect=4")
        tap("T0b", "up", TAP_LENS[4:])
        add("wait 300ms")
        add("uisel")
        at_nav()
        reset(0)
        # into PERFORMANCE (the sixth button of the Settings list)
        goto(5)
        add("press a 40ms")
        add("wait_until ui_count == 19 20s")
        add("wait 1000ms")
        add("log MI page PERFORMANCE")
        add("uisel")
        at_cap()
        add("log MI test T1 key=down n=8 expect=8")
        tap("T1", "down", TAP_LENS)
        add("wait 300ms")
        add("uisel")
        add("log MI test T1b key=up n=8 expect=8")
        tap("T1b", "up", TAP_LENS[::-1])
        add("wait 300ms")
        add("uisel")
        at_nav()
        reset(0)
        at_cap()
        add("log MI test T2 key=down hold=1000 expect=5")
        add("uisel")
        add("state")
        add("press down 1000ms")
        add("wait 700ms")
        add("state")
        add("uisel")
        add("log MI end T2")
        at_nav()
        reset(0)
        goto(4)
        add("uisel")
        at_cap()
        add("log MI test T3 key=right n=6 expect=6")
        add("get mcla_city_lod")
        tap("T3", "right", TAP_LENS[:6], "get mcla_city_lod")
        add("log MI test T3b key=left n=3 expect=3")
        tap("T3b", "left", TAP_LENS[:3], "get mcla_city_lod")
        at_nav()
        add("press_until up ui_select == 1 every=500ms hold=50ms timeout=20s")
        add("wait 400ms")
        add("uisel")
        at_cap()
        add("log MI test T4 key=a n=4 expect=4")
        add("get mcla_fence_yield")
        tap("T4", "a", TAP_LENS[:4], "get mcla_fence_yield")
        at_nav()
        reset(0)
        add("press b 40ms")
        add("wait_until ui_count < 19 15s")
        add("wait 1200ms")
        if not lag and variant in "ABS":
            # DISPLAY is the fifth button of the Settings list (the pause list comes back on PERFORMANCE)
            add("set hdr_peak_nits 0")
            add("press_until up ui_select == 4 every=600ms hold=40ms timeout=30s")
            add("wait 400ms")
            add("press a 40ms")
            add("wait_until ui_count == 15 20s")
            add("wait 1000ms")
            add("log MI page DISPLAY")
            goto(10)
            add("uisel")
            at_cap()
            add("log MI test T5 key=right n=4 expect=4")
            add("get hdr_peak_nits")
            tap("T5", "right", TAP_LENS[:4], "get hdr_peak_nits")
            add("log MI test T5h key=right hold=3000")
            add("get hdr_peak_nits")
            add("press right 3000ms")
            add("wait 700ms")
            add("get hdr_peak_nits")
            add("log MI test T5l key=left hold=3000")
            add("press left 3000ms")
            add("wait 700ms")
            add("get hdr_peak_nits")
            at_nav()
            reset(0)
            add("press b 40ms")
            add("wait_until ui_count != 15 15s")
            add("wait 1200ms")
        add("log MI end cell")
    if fg:
        add("set frame_generation off")
    if lag:
        # The pause menu's opening time (last, so that a failure here loses nothing): Start closes the pause
        # menu, a wait, then Start opens it and `down` is pressed every 50 ms until the list takes it (its
        # selected row becomes 1). The row is 0 beforehand. Quantised to 50 ms, and the first `down` comes
        # 100 ms after Start (the length of its press); the same in every opening.
        add("set mcla_fps 120")
        add("wait 800ms")
        for k, variant in enumerate(LAG_OPEN):
            add(f"set mcla_ui_advance_on_input {'true' if LAG_VARIANTS[variant][0] else 'false'}")
            add(f"set mcla_ui_menu_rate {LAG_VARIANTS[variant][1]}")
            add("press_until up ui_select == 0 every=400ms hold=40ms timeout=15s")
            add("wait 400ms")
            add("press start 100ms")
            add("wait 3000ms")
            add(f"log MI open id={k} variant={variant}")
            add("press start 100ms")
            add("press_until down ui_select == 1 every=50ms hold=30ms timeout=10s")
            add(f"log MI openend id={k}")
            add("wait 1500ms")
    add("mark end")
    return out


# --- report ---

LOG_LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})\] \[\w+\] \[[^\]]*\] \[[^\]]*\] (.*)$")


def log_time_ms(m):
    return ((int(m.group(2)) * 60 + int(m.group(3))) * 60 + int(m.group(4))) * 1000 + int(m.group(5))


def parse_run(path):
    """Returns the cells of one run: a list of dicts with their tests."""
    cells = []
    cell = None
    test = None
    tap = None
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = LOG_LINE.match(raw)
        if not m:
            continue
        t = log_time_ms(m)
        text = m.group(6)
        if text.startswith("dev pad:"):
            body = text[len("dev pad:"):].strip()
            if body.startswith("MI "):
                words = body[3:].split()
                kv = dict(w.split("=", 1) for w in words[1:] if "=" in w)
                kind = words[0]
                if kind == "cell":
                    cell = {"id": int(kv["id"]), "variant": kv["variant"], "cap": int(kv["cap"]),
                            "timing": int(kv["timing"]), "repeat": int(kv["repeat"]),
                            "fg": int(kv["fg"]), "tests": {}, "states": [], "t0": t,
                            "adv_on_input": int(kv["adv"]) if "adv" in kv else None,
                            "menu_rate": int(kv["rate"]) if "rate" in kv else None,
                            "adv": [], "opens": []}
                    cells.append(cell)
                    test = tap = None
                elif kind == "test" and cell is not None:
                    name = words[1]
                    kv = dict(w.split("=", 1) for w in words[2:] if "=" in w)
                    test = {"name": name, "key": kv.get("key", ""), "expect": int(kv.get("expect", 0)),
                            "hold": int(kv.get("hold", 0)), "taps": [], "events": [], "sel": [],
                            "values": [], "t0": t}
                    cell["tests"][name] = test
                    tap = None
                elif kind == "tap" and test is not None:
                    tap = {"i": int(kv["i"]), "len": int(kv["len"]), "events": [], "t": t, "value": None}
                    test["taps"].append(tap)
                elif kind == "tapend":
                    if tap is not None:
                        tap["t_end"] = t
                    tap = None
                elif kind == "open" and cells:
                    cells[-1]["opens"].append({"id": int(kv["id"]), "variant": kv["variant"], "t": t, "t_end": None})
                elif kind == "openend" and cells and cells[-1]["opens"]:
                    cells[-1]["opens"][-1]["t_end"] = t
                elif kind == "end":
                    if words[1:2] == ["cell"] and cell is not None:
                        cell["t1"] = t
                    elif test is not None:
                        test["t1"] = t
                    test = tap = None
            elif body.startswith("uisel ") and test is not None:
                mm = re.search(r"select=(-?\d+) scroll=(-?\d+) count=(-?\d+) rowval=(-?\d+)", body)
                if mm:
                    test["sel"].append((t, int(mm.group(1)), int(mm.group(3))))
            elif body.startswith("get ") and test is not None and tap is not None:
                mm = re.match(r"get (\S+)=(\S+)", body)
                if mm:
                    tap["value"] = mm.group(2)
                    test["values"].append((t, mm.group(1), mm.group(2)))
            elif body.startswith("get ") and test is not None:
                mm = re.match(r"get (\S+)=(\S+)", body)
                if mm:
                    test["values"].append((t, mm.group(1), mm.group(2)))
            elif body.startswith("state") and test is not None:
                mm = re.search(r"frame=(\d+) swaps=(\d+) ms=(\d+)", body)
                if mm:
                    test.setdefault("states", []).append((int(mm.group(1)), int(mm.group(2)), int(mm.group(3))))
        elif text.startswith("ui adv:") and cells:
            mm = re.match(r"ui adv: (\S+) ctx=(\w+) t=(\d+)\.(\d+) ms in=(\d)", text)
            if mm:
                cells[-1]["adv"].append({"name": mm.group(1), "ctx": mm.group(2), "log_ms": t, "in": int(mm.group(5)),
                                         "ms": int(mm.group(3)) + int(mm.group(4)) / 1000.0})
        elif text.startswith("ui key:") and test is not None:
            mm = re.match(r"ui key: (\w+) input=(\d+) key=(-?\d+) t=(\d+)\.(\d+) ms", text)
            if mm:
                event = {"kind": mm.group(1), "input": int(mm.group(2)), "key": int(mm.group(3)),
                         "ms": int(mm.group(4)) + int(mm.group(5)) / 1000.0, "log_ms": t}
                test["events"].append(event)
                if tap is not None:
                    tap["events"].append(event)
    return cells


def cell_fps(cell):
    states = cell["tests"].get("T2", {}).get("states", [])
    if len(states) >= 2:
        (f0, s0, m0), (f1, s1, m1) = states[0], states[-1]
        if m1 > m0:
            return 1000.0 * (f1 - f0) / (m1 - m0), 1000.0 * (s1 - s0) / (m1 - m0)
    return None, None


def describe_cell(cell, lines, verdicts):
    fps, swaps = cell_fps(cell)
    head = (f"cell {cell['id']} {cell['variant']}: cap {cell['cap'] or 'uncapped'}, timing fix "
            f"{'on' if cell['timing'] else 'off'}, repeat fix {'on' if cell['repeat'] else 'off'}"
            f"{', frame generation' if cell['fg'] else ''}")
    if cell.get("adv_on_input") is not None:
        head += f", advance on input {'on' if cell['adv_on_input'] else 'off'}, menu rate {cell['menu_rate']}"
    if fps:
        head += f"  (game {fps:.0f} updates/s, {swaps:.0f} swaps/s during the hold)"
    lines.append(head)
    ok_all = True
    for name in ("T0", "T0b", "T1", "T1b", "T3", "T3b", "T4", "T5"):
        test = cell["tests"].get(name)
        if not test:
            continue
        counts = [len(t["events"]) for t in test["taps"]]
        exact = sum(1 for c in counts if c == 1)
        sel = test["sel"]
        moved = ""
        if len(sel) >= 2 and name in ("T0", "T0b", "T1", "T1b"):
            moved = f"  rows moved {sel[-1][1] - sel[0][1]:+d}"
        values = ""
        if name in ("T3", "T3b", "T4", "T5"):
            values = "  values " + " ".join(v for _, _, v in test["values"][-len(counts) - 1:])
        lines.append(f"  {name:3s} {test['key']:5s} taps {len(counts)}: key events per tap {counts}  "
                     f"exactly one: {exact}/{len(counts)}{moved}{values}")
        if exact != len(counts):
            ok_all = False
    for name in ("T5h", "T5l"):
        hold = cell["tests"].get(name)
        if hold:
            times = [e["ms"] - hold["events"][0]["ms"] for e in hold["events"]] if hold["events"] else []
            gaps = [round(b - a) for a, b in zip(times, times[1:])]
            values = " ".join(v for _, _, v in hold["values"])
            lines.append(f"  {name:3s} held 3 s: {len(times)} key events, first repeat after "
                         f"{gaps[0] if gaps else '-'} ms, then {gaps[1:4]}  hdr_peak_nits {values}")
    t2 = cell["tests"].get("T2")
    if t2:
        times = [e["ms"] - t2["events"][0]["ms"] for e in t2["events"]] if t2["events"] else []
        gaps = [round(b - a) for a, b in zip(times, times[1:])]
        sel = t2["sel"]
        moved = f"  rows moved {sel[-1][1] - sel[0][1]:+d}" if len(sel) >= 2 else ""
        lines.append(f"  T2  hold 1 s: {len(times)} key events (expected 5), first repeat after "
                     f"{gaps[0] if gaps else '-'} ms, gaps {gaps[1:6]}{moved}")
        if len(times) != 5:
            ok_all = False
    verdicts.append((cell, ok_all))


def report(paths, out, echo=True):
    lines = []
    verdicts = []
    for path in paths:
        log = Path(path) / "game.log" if Path(path).is_dir() else Path(path)
        cells = parse_run(log)
        lines.append(f"=== {path}: {len(cells)} cells")
        for cell in cells:
            describe_cell(cell, lines, verdicts)
    lines.append("")
    lines.append("=== VERDICT per cell (every tap exactly one key event, the hold 5 events)")
    for cell, ok in verdicts:
        lines.append(f"{'PASS' if ok else 'FAIL'} cell {cell['id']} {cell['variant']} cap "
                     f"{cell['cap'] or 'uncapped'} timing {cell['timing']} repeat {cell['repeat']} "
                     f"fg {cell['fg']}")
    text = "\n".join(lines) + "\n"
    if out:
        Path(out).write_text(text, encoding="utf-8")
    if echo:
        sys.stdout.write(text)
    return text


# --- the lag report (set "lag"): how soon the focused menu movie advances after a key event ---

FOCUS = "PAUSEMOVIE"  # the pause menu and the pages this fork adds to it (they borrow its list)
LAG_TESTS = ("T0", "T0b", "T1", "T1b", "T3", "T3b", "T4")  # taps, 450 ms apart
LAG_MAX_MS = 100.0  # a later advance than this is not the answer to the event: counted as missed


def _percentile(values, q):
    if not values:
        return None
    v = sorted(values)
    return v[min(len(v) - 1, int(round(q * (len(v) - 1))))]


def _fmt(x, digits=1):
    return "-" if x is None else f"{x:.{digits}f}"


def _focus_name(cell):
    names = {}
    for e in cell["adv"]:
        names[e["name"]] = names.get(e["name"], 0) + 1
    if FOCUS in names:
        return FOCUS, names
    return (max(names, key=names.get) if names else None), names


def _lags(cell, name, test_names):
    """(lags in ms, how many answered by an input advance, missed) of the key events of the given tests."""
    import bisect
    adv = sorted((e["ms"], e["in"]) for e in cell["adv"] if e["name"] == name)
    times = [a[0] for a in adv]
    lags, by_input, missed = [], 0, 0
    for tname in test_names:
        test = cell["tests"].get(tname)
        if not test:
            continue
        for ev in test["events"]:
            i = bisect.bisect_left(times, ev["ms"] - 0.0005)
            if i >= len(times) or times[i] - ev["ms"] > LAG_MAX_MS:
                missed += 1
                continue
            lags.append(times[i] - ev["ms"])
            by_input += adv[i][1]
    return lags, by_input, missed


def _rate(events, t_lo, t_hi, key="log_ms"):
    """advances per second of the events with t_lo <= time < t_hi (ms)."""
    if t_hi <= t_lo:
        return None
    return sum(1 for e in events if t_lo <= e[key] < t_hi) * 1000.0 / (t_hi - t_lo)


def lag_cell(cell):
    """The numbers of one cell: dict."""
    name, names = _focus_name(cell)
    out = {"cell": cell, "focus": name, "names": names}
    if not name:
        return out
    out["tap_lags"], out["tap_by_input"], out["tap_missed"] = _lags(cell, name, LAG_TESTS)
    out["hold_lags"], out["hold_by_input"], out["hold_missed"] = _lags(cell, name, ("T2",))
    rates = {}
    idle = cell["tests"].get("IDLE")
    for n in names:
        ev = [e for e in cell["adv"] if e["name"] == n]
        r = {}
        if idle and idle.get("t1"):
            # a 0.3 s margin at both ends: the last update before the test may be a few ms either side
            r["idle"] = _rate(ev, idle["t0"] + 300, idle["t1"] - 300)
        taps = [cell["tests"][t] for t in ("T1", "T1b") if t in cell["tests"] and cell["tests"][t]["taps"]]
        if taps:
            lo = min(t["taps"][0]["t"] for t in taps)
            hi = max(t["taps"][-1].get("t_end", t["taps"][-1]["t"] + 600) for t in taps)
            r["taps"] = _rate(ev, lo, hi)
            r["taps_in"] = sum(1 for e in ev if lo <= e["log_ms"] < hi and e["in"])
        t2 = cell["tests"].get("T2")
        if t2 and t2["events"]:
            t_first = t2["events"][0]["ms"]
            sel = [e for e in ev if t_first <= e["ms"] < t_first + 1000.0]
            r["hold"] = float(len(sel))
            r["hold_in"] = sum(1 for e in sel if e["in"])
        rates[n] = r
    out["rates"] = rates
    return out


def lag_report(paths, out, echo=True):
    lines = []
    summary = []
    verdicts = []
    opens = {}
    for path in paths:
        log = Path(path) / "game.log" if Path(path).is_dir() else Path(path)
        cells = parse_run(log)
        lines.append(f"=== {path}: {len(cells)} cells")
        for cell in cells:
            describe_cell(cell, lines, verdicts)
            ok = verdicts[-1][1]
            res = lag_cell(cell)
            name = res["focus"]
            label = (f"cell {cell['id']} {cell['variant']} cap {cell['cap'] or 'uncapped'}, advance on input "
                     f"{'on' if cell['adv_on_input'] else 'off'}, menu rate {cell['menu_rate']}")
            for o in cell["opens"]:
                if o["t_end"] is not None:
                    opens.setdefault(o["variant"], []).append(o["t_end"] - o["t"])
            if not name:
                lines.append(f"  NO 'ui adv:' lines in this cell: was --mcla_ui_advance_log=true passed?")
                summary.append((cell, None, ok))
                continue
            tl, hl = res["tap_lags"], res["hold_lags"]
            lines.append(f"  lag of the next advance of {name} after a key event (ms): taps n={len(tl)} median "
                         f"{_fmt(_percentile(tl, 0.5), 2)} p90 {_fmt(_percentile(tl, 0.9), 2)} max {_fmt(max(tl) if tl else None, 2)} "
                         f"(answered by an input advance: {res['tap_by_input']}, no advance within {LAG_MAX_MS:.0f} ms: {res['tap_missed']}); "
                         f"hold n={len(hl)} median {_fmt(_percentile(hl, 0.5), 2)} max {_fmt(max(hl) if hl else None, 2)} "
                         f"(by input {res['hold_by_input']}, missed {res['hold_missed']})")
            for n in sorted(res["names"]):
                if n == name or res["names"][n] < 5:
                    continue
                ol, oi, om = _lags(cell, n, LAG_TESTS)
                lines.append(f"  (other screen {n}: lag of its next advance after a tap: n={len(ol)} median "
                             f"{_fmt(_percentile(ol, 0.5), 2)} max {_fmt(max(ol) if ol else None, 2)}, by input {oi}, missed {om})")
            for n, r in sorted(res["rates"].items()):
                lines.append(f"  advances a second of {n}: idle {_fmt(r.get('idle'))}, taps every 450 ms {_fmt(r.get('taps'))} "
                             f"(input advances {r.get('taps_in', '-')}), in the 1 s hold {_fmt(r.get('hold'), 0)} "
                             f"(input advances {r.get('hold_in', '-')})")
            summary.append((cell, res, ok))
    lines.append("")
    lines.append("=== SUMMARY: the focused menu movie (PAUSEMOVIE: the pause menu and its pages)")
    lines.append("cell  variant  cap       adv-in  rate | lag taps ms: n  median   p90    max | hold: median   max | "
                 "adv/s: idle   taps   hold | input check")
    for cell, res, ok in summary:
        if not res or not res.get("focus"):
            lines.append(f"{cell['id']:4d}  {cell['variant']:7s}  {cell['cap'] or 'uncapped':8}  no advance lines")
            continue
        tl, hl = res["tap_lags"], res["hold_lags"]
        r = res["rates"].get(res["focus"], {})
        lines.append(f"{cell['id']:4d}  {cell['variant']:7s}  {str(cell['cap'] or 'uncapped'):8}  {cell['adv_on_input']:6d}  "
                     f"{cell['menu_rate']:4d} | {len(tl):15d} {_fmt(_percentile(tl, 0.5), 2):>7} {_fmt(_percentile(tl, 0.9), 2):>6} "
                     f"{_fmt(max(tl) if tl else None, 2):>6} | {_fmt(_percentile(hl, 0.5), 2):>11} {_fmt(max(hl) if hl else None, 2):>5} | "
                     f"{_fmt(r.get('idle')):>10} {_fmt(r.get('taps')):>6} {_fmt(r.get('hold'), 0):>6} | {'PASS' if ok else 'FAIL'}")
    lines.append("")
    lines.append("=== The pause menu's opening (Start pressed, to the first `down` the list took; 50 ms steps, +100 ms for the press), at a 120 FPS cap")
    for variant in sorted(opens):
        v = opens[variant]
        lines.append(f"  {variant} (advance on input {'on' if LAG_VARIANTS[variant][0] else 'off'}): "
                     f"{', '.join(f'{x:.0f}' for x in v)} ms, mean {sum(v) / len(v):.0f}")
    if not opens:
        lines.append("  no opening was measured (the script ended before it, or its markers are missing)")
    lines.append("")
    lines.append("=== VERDICT")
    on_cells = [(c, ok) for c, res, ok in summary if c["adv_on_input"]]
    off_cells = [(c, ok) for c, res, ok in summary if not c["adv_on_input"]]
    bad = [c for c, ok in on_cells if not ok]
    lines.append(f"{'PASS' if on_cells and not bad else 'FAIL'} input check on the cells with advance on input "
                 f"({len(on_cells) - len(bad)} of {len(on_cells)} cells: every tap one key event, the hold 5)")
    if off_cells:
        bad_off = [c for c, ok in off_cells if not ok]
        lines.append(f"{'OK' if not bad_off else 'NOTE'} the same check with it off: {len(off_cells) - len(bad_off)} of {len(off_cells)} cells pass")
    by_cap = {}
    for cell, res, ok in summary:
        if res and res.get("focus") and res["tap_lags"]:
            by_cap.setdefault(cell["cap"], {})[cell["variant"]] = (res, cell)
    for cap, d in sorted(by_cap.items()):
        if "A" in d and "B" in d:
            ma = _percentile(d["A"][0]["tap_lags"], 0.5)
            mb = _percentile(d["B"][0]["tap_lags"], 0.5)
            xb = max(d["B"][0]["tap_lags"])
            hb = d["B"][0]["rates"][d["B"][0]["focus"]].get("hold")
            ib = d["B"][0]["rates"][d["B"][0]["focus"]].get("idle")
            good = mb is not None and ma is not None and mb <= 3.0 and xb <= 12.0 and mb < ma
            lines.append(f"{'GOOD' if good else 'CHECK'} cap {cap or 'uncapped'}: median lag A {_fmt(ma, 2)} ms, B {_fmt(mb, 2)} ms, "
                         f"B max {_fmt(xb, 2)} ms; B idle {_fmt(ib)} advances/s, B 1 s hold {_fmt(hb, 0)} advances "
                         f"(wanted: B median <= 3 ms, max <= 12 ms, idle 28 to 32, hold <= 36)")
    if "A" in opens and "B" in opens:
        ma = sum(opens["A"]) / len(opens["A"])
        mb = sum(opens["B"]) / len(opens["B"])
        lines.append(f"{'GOOD' if abs(mb - ma) <= 150 else 'CHECK'} pause menu opening: A mean {ma:.0f} ms, B mean {mb:.0f} ms "
                     f"(wanted: within 150 ms, the measure's own step is 50 ms)")
    text = "\n".join(lines) + "\n"
    if out:
        Path(out).write_text(text, encoding="utf-8")
    if echo:
        sys.stdout.write(text)
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("pad")
    a.add_argument("out")
    a.add_argument("--set", default="res1", choices=["res1", "res3", "fg", "merged", "lag"])
    b = sub.add_parser("report")
    b.add_argument("runs", nargs="+")
    b.add_argument("--out")
    c = sub.add_parser("lagreport")
    c.add_argument("runs", nargs="+")
    c.add_argument("--out")
    args = p.parse_args()
    if args.cmd == "pad":
        Path(args.out).write_text("\n".join(pad_lines(args.set)) + "\n", encoding="ascii")
        print(f"wrote {args.out}")
    elif args.cmd == "lagreport":
        lag_report(args.runs, args.out)
    else:
        report(args.runs, args.out)


if __name__ == "__main__":
    main()

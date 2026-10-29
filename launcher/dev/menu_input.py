"""Menu input check: how many steps one press makes (fork-only development tool).

  menu_input.py pad <out.pad> --set res1|res3|fg      write the dev pad script of one run
  menu_input.py report <run folder>... [--out FILE]    read the runs' game.log and say what each press did

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
CAPS = [30, 60, 120, 0]
# (timing fix, repeat fix): A = as shipped in v0.3.0, B = the repeat fix, C = the timing fix off (the
# workaround), D = both off the shipped way round (timing fix off, repeat fix on).
VARIANTS = {"A": (1, 0), "B": (1, 1), "C": (0, 0), "D": (0, 1)}

KEY_NAMES = {47: "dpad up", 48: "dpad down", 51: "dpad left", 52: "dpad right", 49: "stick up",
             50: "stick down", 53: "stick left", 54: "stick right", 55: "A", 56: "B"}


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
    else:
        raise SystemExit(f"unknown set {run_set}")
    if fg:
        add("set frame_generation fsr")
        add("get frame_generation fg_status mcla_fps mcla_fps_frame_generation")

    for n, (cap, variant) in enumerate(cells):
        timing, repeat = VARIANTS[variant]
        add(f"# --- cell {n}: cap {cap}, timing fix {timing}, repeat fix {repeat}, frame generation {fg}")
        add(f"set mcla_fps {cap}")
        if fg:
            add(f"set mcla_fps_frame_generation {cap}")
        add(f"set mcla_ui_timing_fix {'true' if timing else 'false'}")
        add(f"set mcla_ui_repeat_fix {'true' if repeat else 'false'}")
        add("wait 1500ms")
        add(f"log MI cell id={n} variant={variant} cap={cap} timing={timing} repeat={repeat} fg={fg}")
        add("state")
        reset(0)
        # T0: the pause menu's own list
        add("log MI test T0 key=down n=4 expect=4")
        add("uisel")
        tap("T0", "down", TAP_LENS[:4])
        add("wait 300ms")
        add("uisel")
        add("log MI test T0b key=up n=4 expect=4")
        tap("T0b", "up", TAP_LENS[4:])
        add("wait 300ms")
        add("uisel")
        reset(0)
        # into PERFORMANCE (the sixth button of the Settings list)
        goto(5)
        add("press a 40ms")
        add("wait_until ui_count == 19 20s")
        add("wait 1000ms")
        add("log MI page PERFORMANCE")
        add("uisel")
        add("log MI test T1 key=down n=8 expect=8")
        tap("T1", "down", TAP_LENS)
        add("wait 300ms")
        add("uisel")
        add("log MI test T1b key=up n=8 expect=8")
        tap("T1b", "up", TAP_LENS[::-1])
        add("wait 300ms")
        add("uisel")
        reset(0)
        add("log MI test T2 key=down hold=1000 expect=5")
        add("uisel")
        add("press down 1000ms")
        add("wait 700ms")
        add("uisel")
        add("log MI end T2")
        reset(0)
        goto(4)
        add("uisel")
        add("log MI test T3 key=right n=6 expect=6")
        add("get mcla_city_lod")
        tap("T3", "right", TAP_LENS[:6], "get mcla_city_lod")
        add("log MI test T3b key=left n=3 expect=3")
        tap("T3b", "left", TAP_LENS[:3], "get mcla_city_lod")
        add("press_until up ui_select == 1 every=500ms hold=50ms timeout=20s")
        add("wait 400ms")
        add("uisel")
        add("log MI test T4 key=a n=4 expect=4")
        add("get mcla_fence_yield")
        tap("T4", "a", TAP_LENS[:4], "get mcla_fence_yield")
        reset(0)
        add("press b 40ms")
        add("wait_until ui_count < 19 15s")
        add("wait 1200ms")
        if variant in "AB":
            # DISPLAY is the fifth button of the Settings list (the pause list comes back on PERFORMANCE)
            add("press_until up ui_select == 4 every=600ms hold=40ms timeout=30s")
            add("wait 400ms")
            add("press a 40ms")
            add("wait_until ui_count == 15 20s")
            add("wait 1000ms")
            add("log MI page DISPLAY")
            goto(10)
            add("uisel")
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
            reset(0)
            add("press b 40ms")
            add("wait_until ui_count != 15 15s")
            add("wait 1200ms")
        add("state")
        add("log MI end cell")
    if fg:
        add("set frame_generation off")
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
                            "fg": int(kv["fg"]), "tests": {}, "states": [], "t0": t}
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
                elif kind in ("end", "page", "setup"):
                    if kind == "end" and words[1:2] == ["cell"] and cell is not None:
                        cell["t1"] = t
                    tap = None if kind == "end" else tap
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
            elif body.startswith("state") and cell is not None:
                mm = re.search(r"frame=(\d+) swaps=(\d+) ms=(\d+)", body)
                if mm:
                    cell["states"].append((int(mm.group(1)), int(mm.group(2)), int(mm.group(3))))
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
    if len(cell["states"]) >= 2:
        (f0, s0, m0), (f1, s1, m1) = cell["states"][0], cell["states"][-1]
        if m1 > m0:
            return 1000.0 * (f1 - f0) / (m1 - m0), 1000.0 * (s1 - s0) / (m1 - m0)
    return None, None


def describe_cell(cell, lines, verdicts):
    fps, swaps = cell_fps(cell)
    head = (f"cell {cell['id']} {cell['variant']}: cap {cell['cap'] or 'uncapped'}, timing fix "
            f"{'on' if cell['timing'] else 'off'}, repeat fix {'on' if cell['repeat'] else 'off'}"
            f"{', frame generation' if cell['fg'] else ''}")
    if fps:
        head += f"  (game {fps:.0f} updates/s, {swaps:.0f} swaps/s over the cell)"
    lines.append(head)
    ok_all = True
    for name in ("T0", "T0b", "T1", "T1b", "T3", "T3b", "T4"):
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
        if name in ("T3", "T3b", "T4"):
            values = "  values " + " ".join(v for _, _, v in test["values"][-len(counts) - 1:])
        lines.append(f"  {name:3s} {test['key']:5s} taps {len(counts)}: key events per tap {counts}  "
                     f"exactly one: {exact}/{len(counts)}{moved}{values}")
        if exact != len(counts):
            ok_all = False
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


def report(paths, out):
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
    sys.stdout.write(text)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("pad")
    a.add_argument("out")
    a.add_argument("--set", default="res1", choices=["res1", "res3", "fg"])
    b = sub.add_parser("report")
    b.add_argument("runs", nargs="+")
    b.add_argument("--out")
    args = p.parse_args()
    if args.cmd == "pad":
        Path(args.out).write_text("\n".join(pad_lines(args.set)) + "\n", encoding="ascii")
        print(f"wrote {args.out}")
    else:
        report(args.runs, args.out)


if __name__ == "__main__":
    main()

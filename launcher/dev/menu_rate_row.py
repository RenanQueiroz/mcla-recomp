"""The pause menu's MENU UPDATES row (mcla_ui_menu_rate): its pads and its report (fork-only development tool).

  menu_rate_row.py pad <out.pad> --kind values --shots <folder> [--scale 0.5]   pictures of the row's four values
  menu_rate_row.py pad <out.pad> --kind function                                 set the row from the menu, read what it did
  menu_rate_row.py report <run folder> [--start-toml FILE] [--out FILE]          read the function run's game.log

values: pause > Settings > DISPLAY, the row selected (it is the fifth, under HUD UPDATES), a picture at each of its values
(30, 60, 120 PER SECOND, EVERY FRAME) with the left/right button; `get` after each.

function (run it at --res 1 with --mcla_ui_advance_log=true; the pad sets the frame cap to 120 itself):
  - the row is stepped with right (30, 60, 120, EVERY FRAME, then round to 30) and left (EVERY FRAME, 120, 60), and
    `get mcla_ui_menu_rate` after each step says what the menu wrote;
  - at 30, 60, 120 and EVERY FRAME it waits 3 s with the page open; the log's `ui adv: PAUSEMOVIE` lines in that time
    are how often the pause menu's movie advanced (30, 60, 120, and once per game update), which says the change took
    effect at once;
  - it leaves the page at 60: the menu saves what the player changed, and the report compares the run's mcla.toml
    (`mcla.toml.after` in the run folder) with the file the run started with (--toml of devrun): only
    `mcla_ui_menu_rate = 60` may be new.
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from menu_input import LOG_LINE, log_time_ms  # noqa: E402

ROW = 4  # MENU UPDATES: FPS TARGET, FRAME GENERATION, the latency reducer, HUD UPDATES, then this row
ROWS = 16  # DISPLAY's rows with it (15 before)
RATES = [30, 60, 120, 0]  # the row's values, in its order; 0 is EVERY FRAME
STEPS = [("right", 60), ("right", 120), ("right", 0), ("right", 30), ("left", 0), ("left", 120), ("left", 60)]
WINDOWS = 3000  # ms the page stays open at each of the four rates
NAME = "PAUSEMOVIE"


def _boot(add):
    add("wait 8s")
    add("mark title")
    add("press_until start left_title every=2s timeout=90s")
    add("mark map_zoom")
    add("wait_until roam 180s")
    add("wait 8s")
    add("mark free_roam")


def pad_values(shots, scale):
    out = []
    add = out.append
    add("# Made by menu_rate_row.py (see its docstring). Do not edit: change the generator.")
    _boot(add)
    add("log MRR values: xbox prompts")
    add("set mcla_button_prompts xbox")
    add("wait 600ms 36f")
    add("press start 4f")
    add("wait 1500ms 90f")
    for _ in range(2):  # the Settings tab
        add("press rb 4f")
        add("wait 1000ms 60f")
    add("press_until down ui_select == 4 every=600ms hold=40ms timeout=30s")  # DISPLAY, the fifth button
    add("wait 400ms 24f")
    add("press a 4f")
    add(f"wait_until ui_count == {ROWS} 20s")
    add("wait 1500ms 90f")
    add(f"log MRR page DISPLAY rows={ROWS}")
    add(f"press_until down ui_select == {ROW} every=600ms hold=40ms timeout=30s")
    add("wait 800ms 48f")
    names = {30: "30", 60: "60", 120: "120", 0: "every"}
    for i, rate in enumerate(RATES):
        if i:
            add("press right 4f")
            add("wait 1500ms 90f")
        add(f"log MRR value={rate}")
        add("uisel")
        add("get mcla_ui_menu_rate")
        add(f"shot {shots}/rate-{names[rate]}.png {scale}")
    add("press right 4f")  # round to 30
    add("wait 1000ms 60f")
    add("get mcla_ui_menu_rate")
    add("press b 4f")
    add("wait 1500ms 90f")
    add("press start 4f")
    add("wait 1500ms 90f")
    add("mark end")
    return out


def pad_function():
    out = []
    add = out.append
    add("# Made by menu_rate_row.py (see its docstring). Do not edit: change the generator.")
    _boot(add)
    add("press start 100ms")
    add("wait 4500ms")
    add("wait_until ui_count >= 0 20s")
    add("press rb 40ms")
    add("wait 1200ms")
    add("press rb 40ms")
    add("wait 1500ms")
    add("log MRR setup the Settings tab")
    add("uisel")
    add("press_until down ui_select == 4 every=600ms hold=40ms timeout=30s")  # DISPLAY, the fifth button
    add("wait 400ms")
    add("press a 40ms")
    add(f"wait_until ui_count == {ROWS} 20s")
    add("wait 1000ms")
    add(f"log MRR page DISPLAY rows={ROWS}")
    add(f"press_until down ui_select == {ROW} every=600ms hold=40ms timeout=30s")
    add("wait 400ms")
    add("set mcla_fps 120")
    add("wait 800ms")
    add("uisel")

    def window(i, rate):
        add(f"log MRR step value={rate}")
        add("get mcla_ui_menu_rate")
        add("uisel")
        add(f"log MRR window id={i} rate={rate}")
        add("state")
        add(f"wait {WINDOWS}ms")
        add("state")
        add(f"log MRR end id={i}")

    window(0, RATES[0])
    for i, (key, rate) in enumerate(STEPS):
        add(f"log MRR step key={key} value={rate}")
        add(f"press {key} 40ms")
        add("wait 700ms")
        if i < 3:  # right from 30 to 60, 120 and EVERY FRAME: how often the movie advances at each
            window(i + 1, rate)
        else:
            add("get mcla_ui_menu_rate")
            add("uisel")
    add("log MRR leave the page")
    add("press b 40ms")
    add("wait 1500ms")
    add("get mcla_ui_menu_rate")
    add("wait 500ms")
    add("mark end")
    return out


# --- report ---

def parse_run(path):
    r"""The run's log: windows (start and end time, the game's updates in them), PAUSEMOVIE advances, the values `get` read,
    what the menu set and saved."""
    run = {"windows": [], "adv": [], "gets": [], "sets": [], "saved": [], "pad_ok": False, "page": None, "layout": {}}
    window = None
    step = None
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = LOG_LINE.match(raw)
        if not m:
            continue
        t = log_time_ms(m)
        text = m.group(6)
        if text.startswith("dev pad:"):
            body = text[len("dev pad:"):].strip()
            if body.startswith("MRR "):
                words = body[4:].split()
                kv = dict(w.split("=", 1) for w in words[1:] if "=" in w)
                if words[0] == "window":
                    window = {"id": int(kv["id"]), "rate": int(kv["rate"]), "t0": t, "t1": None, "states": []}
                    run["windows"].append(window)
                elif words[0] == "end" and window is not None:
                    window["t1"] = t
                    window = None
                elif words[0] == "step":
                    step = kv.get("value")
                elif words[0] == "page":
                    run["page"] = int(kv.get("rows", 0))
            elif body.startswith("state ->") and window is not None:
                mm = re.search(r"frame=(\d+) swaps=(\d+) ms=(\d+)", body)
                if mm:
                    window["states"].append((int(mm.group(1)), int(mm.group(3))))
            elif body.startswith("get mcla_ui_menu_rate="):
                run["gets"].append((t, body.split("=", 1)[1].strip(), step))
            elif body.startswith("script ") and " done: " in body:
                run["pad_ok"] = body.endswith("done: ok") or " done: ok" in body
        elif text.startswith("ui adv: "):
            mm = re.match(r"ui adv: (\S+) ctx=(\w+) t=(\d+)\.(\d+) ms in=(\d)", text)
            if mm:
                run["adv"].append((t, mm.group(1), int(mm.group(3)) + int(mm.group(4)) / 1000.0))
        elif text.startswith("settings menu: mcla_ui_menu_rate = "):
            run["sets"].append((t, text.split("=", 1)[1].strip()))
        elif "setting(s) saved to" in text:
            run["saved"].append(text)
        else:
            mm = re.search(r"settings menu: '([^']+)' layout: menu\.width (\d+) height (\d+) count (\d+)", text)
            if mm:
                run["layout"][mm.group(1)] = (int(mm.group(2)), int(mm.group(4)))
    return run


def toml_lines(path):
    return [line.strip() for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()]


def report(folder, start_toml=None, out=None, echo=True):
    """The PASS/FAIL lines of a function run; returns (text, all_ok)."""
    folder = Path(folder)
    run = parse_run(folder / "game.log")
    lines = [f"== {folder.name}: the MENU UPDATES row (function run)"]
    verdict = []

    def check(name, ok, detail):
        lines.append(f"   {'PASS' if ok else 'FAIL'} {name}: {detail}")
        verdict.append(ok)

    check("pad", run["pad_ok"], "the script ran to its end" if run["pad_ok"] else "the script did not end ok (see game.log)")
    check("rows", run["page"] == ROWS, f"DISPLAY opened with {run['page']} rows (expected {ROWS})")
    # what the menu wrote, step by step
    values = [int(v) if re.fullmatch(r"-?\d+", v) else v for _, v, _ in run["gets"]]
    # 30 (before the first window), one at each of the next three windows, one after each of the other four steps, and one
    # after leaving the page: 30 60 120 0 | 30 0 120 60 | 60
    expect = [30, 60, 120, 0, 30, 0, 120, 60, 60]
    check("steps", values == expect, f"`get mcla_ui_menu_rate` after each step: {values} (expected {expect}; right goes round from EVERY FRAME to 30, left from 30 to EVERY FRAME)")
    set_values = [v for _, v in run["sets"]]
    check("set by the menu", set_values == [str(v) for _, v in STEPS],
          f"the menu's own log lines: {set_values}")
    # how often the pause menu's movie advanced at each
    for w in run["windows"]:
        dur = (w["t1"] - w["t0"]) / 1000.0 if w["t1"] else 0
        advs = [(t, ms) for t, name, ms in run["adv"] if name == NAME and w["t0"] <= t <= (w["t1"] or w["t0"])]
        per_s = len(advs) / dur if dur else 0
        fps = None
        if len(w["states"]) >= 2 and w["states"][-1][1] > w["states"][0][1]:
            (f0, m0), (f1, m1) = w["states"][0], w["states"][-1]
            fps = 1000.0 * (f1 - f0) / (m1 - m0)
        rate = w["rate"]
        if fps is None:
            check(f"advances at {rate or 'EVERY FRAME'}", False, "no game update count for the window")
            continue
        want = fps if rate == 0 else min(float(rate), fps)
        ok = abs(per_s - want) <= max(0.1 * want, 1.5)
        label = "EVERY FRAME" if rate == 0 else f"{rate} PER SECOND"
        check(f"advances at {label}", ok,
              f"{NAME} advanced {len(advs)} times in {dur:.2f} s = {per_s:.1f}/s; the game ran {fps:.1f} updates/s, "
              f"so about {want:.1f}/s expected (within 10 percent)")
    other = {}
    for _, name, _ in run["adv"]:
        other[name] = other.get(name, 0) + 1
    lines.append(f"   advances by screen over the whole run: {other}")
    # the save
    after = folder / "mcla.toml.after"
    if not after.exists():
        check("saved", False, "no mcla.toml.after in the run folder: the menu saved nothing")
    else:
        new = toml_lines(after)
        old = toml_lines(start_toml) if start_toml and Path(start_toml).exists() else []
        added = [x for x in new if x not in old]
        removed = [x for x in old if x not in new]
        lines.append(f"   the run's mcla.toml before ({len(old)} lines): {old}")
        lines.append(f"   after ({len(new)} lines): {new}")
        check("saved", added == ["mcla_ui_menu_rate = 60"] and not removed,
              f"new lines {added}, lines gone {removed} (expected only 'mcla_ui_menu_rate = 60'); log: {run['saved'][-1:] }")
    ok = all(verdict)
    lines.append(f"{'PASS' if ok else 'FAIL'} {folder.name}: the row steps through its four values, takes effect at once (the movie's advance rate follows) and saves only mcla_ui_menu_rate")
    text = "\n".join(lines)
    if out:
        Path(out).write_text(text + "\n", encoding="utf-8")
    if echo:
        print(text)
    return text, ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pad")
    p.add_argument("out")
    p.add_argument("--kind", choices=["values", "function"], required=True)
    p.add_argument("--shots", default="{SHOT}")
    p.add_argument("--scale", default="0.5")
    r = sub.add_parser("report")
    r.add_argument("folder")
    r.add_argument("--start-toml")
    r.add_argument("--out")
    a = ap.parse_args()
    if a.cmd == "pad":
        lines = pad_values(a.shots, a.scale) if a.kind == "values" else pad_function()
        Path(a.out).write_text("\n".join(lines) + "\n", encoding="ascii")
        print(f"{a.out}: {len(lines)} lines")
        return 0
    _, ok = report(a.folder, a.start_toml, a.out)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

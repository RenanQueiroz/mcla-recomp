"""The pause menu's SMOOTH UI row (mcla_ui_interpolate): its pad and its report (fork-only development tool).

It replaced the two rows HUD UPDATES (mcla_ui_hud_rate) and MENU UPDATES (mcla_ui_menu_rate) of the DISPLAY page (v0.3.1).

  smooth_ui_row.py pad <out.pad> --kind function --shots <folder> [--scale 1]   toggle the row from the menu, read what it did
  smooth_ui_row.py report <run folder> [--start-toml FILE] [--out FILE]         read the function run's game.log

function (run it at --res 1 with --mcla_ui_interp_stats=true; the pad sets the frame cap to 120 itself):
  - DISPLAY has 15 rows and the row is the fourth (FPS TARGET, FRAME GENERATION, LOW LATENCY, SMOOTH UI);
  - the option starts ON (the default); the pad presses A on the row three times (OFF, ON, OFF) and `get`s
    mcla_ui_interpolate after each press; a picture of the row is taken at ON and at OFF;
  - after each press the page stays open for 3.5 s: the game logs once a second `ui interp cost: option=on|off ... applied=N`
    (mcla_ui_interp_stats), which says whether the hook saw the change at once (option=) and whether it drew interpolated
    (applied > 0 with the option on, 0 with it off);
  - it leaves the page with the option OFF: the menu saves what the player changed, and the report compares the run's mcla.toml
    (`mcla.toml.after` in the run folder) with the file the run started with (--toml of devrun): only
    `mcla_ui_interpolate = false` may be new.
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from menu_input import LOG_LINE, log_time_ms  # noqa: E402

ROW = 3  # SMOOTH UI: FPS TARGET, FRAME GENERATION, LOW LATENCY, then this row
ROWS = 15  # DISPLAY's rows
WINDOW = 3500  # ms the page stays open at each of the four states
STATES = ["on", "off", "on", "off"]  # the states, in order; the pad presses A between them


def _boot(add):
    add("wait 8s")
    add("mark title")
    add("press_until start left_title every=2s timeout=90s")
    add("mark map_zoom")
    add("wait_until roam 180s")
    add("wait 8s")
    add("mark free_roam")


def pad_function(shots="{SHOT}", scale="1"):
    out = []
    add = out.append
    add("# Made by smooth_ui_row.py (see its docstring). Do not edit: change the generator.")
    _boot(add)
    add("press start 100ms")
    add("wait 4500ms")
    add("wait_until ui_count >= 0 20s")
    add("press rb 40ms")
    add("wait 1200ms")
    add("press rb 40ms")
    add("wait 1500ms")
    add("log SUI setup the Settings tab")
    add("uisel")
    add("press_until down ui_select == 4 every=600ms hold=40ms timeout=30s")  # DISPLAY, the fifth button
    add("wait 400ms")
    add("press a 40ms")
    add(f"wait_until ui_count == {ROWS} 20s")
    add("wait 1000ms")
    add(f"log SUI page DISPLAY rows={ROWS}")
    add(f"press_until down ui_select == {ROW} every=600ms hold=40ms timeout=30s")
    add("wait 400ms")
    add("set mcla_fps 120")
    add("wait 800ms")
    add("uisel")
    for i, state in enumerate(STATES):
        if i:
            add("log SUI step key=a")
            add("press a 40ms")
            add("wait 1200ms")  # the statistics line of the second the press fell in has both states: the report skips it
        add(f"log SUI window id={i} state={state}")
        add("get mcla_ui_interpolate")
        add("uisel")
        if i < 2:
            add(f"shot {shots}/smooth-{state}.png {scale}")
        add(f"wait {WINDOW}ms")
        add(f"log SUI end id={i}")
    add("log SUI leave the page")
    add("press b 40ms")
    add("wait 1500ms")
    add("get mcla_ui_interpolate")
    add("wait 500ms")
    add("mark end")
    return out


# --- report ---

RE_COST = re.compile(r"ui interp cost: option=(on|off) calls=(\d+) total_us=(\d+) per_call_ns=(\d+) worst_us=([\d.]+) gliding=(\d+) applied=(\d+)")


def parse_run(path):
    r"""The run's log: the windows (start and end time, the state the pad expects), the hook's statistics lines, the values `get` read,
    what the menu set and saved."""
    run = {"windows": [], "cost": [], "gets": [], "sets": [], "saved": [], "pad_ok": False, "page": None, "layout": {}}
    window = None
    for raw in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = LOG_LINE.match(raw)
        if not m:
            continue
        t = log_time_ms(m)
        text = m.group(6)
        if text.startswith("dev pad:"):
            body = text[len("dev pad:"):].strip()
            if body.startswith("SUI "):
                words = body[4:].split()
                kv = dict(w.split("=", 1) for w in words[1:] if "=" in w)
                if words[0] == "window":
                    window = {"id": int(kv["id"]), "state": kv["state"], "t0": t, "t1": None}
                    run["windows"].append(window)
                elif words[0] == "end" and window is not None:
                    window["t1"] = t
                    window = None
                elif words[0] == "page":
                    run["page"] = int(kv.get("rows", 0))
            elif body.startswith("get mcla_ui_interpolate="):
                run["gets"].append((t, body.split("=", 1)[1].strip()))
            elif body.startswith("script ") and " done: " in body:
                run["pad_ok"] = " done: ok" in body
        elif "ui interp cost:" in text:
            mc = RE_COST.search(text)
            if mc:
                run["cost"].append((t, mc.group(1), int(mc.group(2)), int(mc.group(6)), int(mc.group(7))))
        elif text.startswith("settings menu: mcla_ui_interpolate = "):
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
    lines = [f"== {folder.name}: the SMOOTH UI row (function run)"]
    verdict = []

    def check(name, ok, detail):
        lines.append(f"   {'PASS' if ok else 'FAIL'} {name}: {detail}")
        verdict.append(ok)

    check("pad", run["pad_ok"], "the script ran to its end" if run["pad_ok"] else "the script did not end ok (see game.log)")
    check("rows", run["page"] == ROWS, f"DISPLAY opened with {run['page']} rows (expected {ROWS})")
    # what the menu wrote, press by press: the default (on) before the first press, then off, on, off, and off after leaving the page
    values = [v for _, v in run["gets"]]
    expect = ["true", "false", "true", "false", "false"]
    check("steps", values == expect, f"`get mcla_ui_interpolate`: {values} (expected {expect})")
    set_values = [v for _, v in run["sets"]]
    check("set by the menu", set_values == ["false", "true", "false"], f"the menu's own log lines: {set_values}")
    # what the hook did in each state: once a second `ui interp cost` says the option it saw and how many draws were interpolated
    for w in run["windows"]:
        if not w["t1"]:
            check(f"window {w['id']}", False, "the window did not end")
            continue
        inside = [c for c in run["cost"] if w["t0"] + 1500 <= c[0] <= w["t1"]]  # whole seconds of this state only
        if not inside:
            check(f"hook {w['state']} (window {w['id']})", False,
                  "no `ui interp cost` line in the window (run with --mcla_ui_interp_stats=true)")
            continue
        options = {c[1] for c in inside}
        applied = [c[4] for c in inside]
        want_on = w["state"] == "on"
        ok = options == {w["state"]} and (min(applied) > 0 if want_on else max(applied) == 0)
        check(f"hook {w['state']} (window {w['id']})", ok,
              f"{len(inside)} statistics lines, option {sorted(options)}, draws interpolated a second {applied} "
              f"(expected {'some' if want_on else 'none'}), glide candidates a second {[c[3] for c in inside]}")
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
        check("saved", added == ["mcla_ui_interpolate = false"] and not removed,
              f"new lines {added}, lines gone {removed} (expected only 'mcla_ui_interpolate = false'); log: {run['saved'][-1:]}")
    ok = all(verdict)
    lines.append(f"{'PASS' if ok else 'FAIL'} {folder.name}: the SMOOTH UI row toggles mcla_ui_interpolate, takes effect at once "
                 f"(the hook sees the option and stops or starts interpolating) and saves only that key")
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
    p.add_argument("--kind", choices=["function"], required=True)
    p.add_argument("--shots", default="{SHOT}")
    p.add_argument("--scale", default="1")
    r = sub.add_parser("report")
    r.add_argument("folder")
    r.add_argument("--start-toml")
    r.add_argument("--out")
    a = ap.parse_args()
    if a.cmd == "pad":
        lines = pad_function(a.shots, a.scale)
        Path(a.out).write_text("\n".join(lines) + "\n", encoding="ascii")
        print(f"{a.out}: {len(lines)} lines")
        return 0
    _, ok = report(a.folder, a.start_toml, a.out)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

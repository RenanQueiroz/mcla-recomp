r"""Checks the log of a run of launcher/dev/scripts/vibration.pad (src/vibration.cpp, mcla_vibration).

  python -I vibration_check.py <run folder or game.log> [--summary FILE]

The run is started with --mcla_vibration=false --mcla_vibration_log=true --mcla_menu_log_layout=true and the dev pad's
synthetic device as the controller. The script makes the game's own rumble function run (mcla_vibration_probe) and turns
the option on and off; the dev pad's device counts every call a driver of the SDK's input system gets
("vibration: driver call #n left=.. right=..") and the module logs what it did with each call of the game. This reads the
lines between the script's "dev pad: log VIB <step>" lines and compares them with what each step must show:

  * the number and values of the driver calls of each step (an option that is off must give none of the game's rumble,
    and none at all once the drivers hold 0,0; one that is on must give every call; turning it off while a motor runs must
    give one call with 0,0 from the update, and turning it on again the rumble the game set meanwhile);
  * what the module did with each call of the game (held back / to the drivers / sent as a stop);
  * the pause menu's rows (CONTROLS: VIBRATION and ADAPTIVE TRIGGERS turn the options, in the log and in the file the
    menu saved: mcla.toml.after, when the folder has it) and the width of the CONTROLS page (the cap is 750 menu units, and
    no page may be sent to a shorter wording by the new rows).

What it cannot show: that SDL or XInput really stop a motor (the device is synthetic), and how any of it feels.
Exit 0 when every check passes.
"""
import argparse
import pathlib
import re
import sys

LINE = re.compile(r"^\[[^\]]*\] \[(?P<level>\w+)\] \[[^\]]*\] \[[^\]]*\] (?P<msg>.*)$")

# step -> the driver calls (left, right) it must show, in order
CALLS = {
    "1": [], "2": [], "3": [],
    "3b": [(40000, 10000)],
    "4": [(0, 0)],
    "5": [(65535, 65535)],
    "6": [(0, 0)],
    "7": [], "8": [],
    "9": [], "10": [],
    "11": [(30000, 30000)],
    "12": [(0, 0)],
    "13": [], "14": [],
}
# step -> what the module must have decided about the game's calls, in order: (left, right, decision)
GUEST = {
    "1": [(32768, 49152, "held back")],
    "2": [(0, 0, "held back")],
    "3": [(40000, 10000, "held back")],
    "4": [(0, 0, "to the drivers")],
    "5": [(65535, 65535, "to the drivers")],
    "7": [(0, 0, "held back")],
    "8": [(20000, 20000, "held back"), (0, 0, "held back")],
    "10": [(30000, 30000, "held back")],
    "12": [(0, 0, "to the drivers")],
}
# step -> the sends the update made without a call from the game: (left, right, reason)
SENT = {
    "3b": [(40000, 10000, "switched on")],
    "6": [(0, 0, "switched off")],
    "11": [(30000, 30000, "switched on")],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--summary", default="")
    a = ap.parse_args()
    path = pathlib.Path(a.path)
    folder = path if path.is_dir() else path.parent
    log = folder / "game.log" if path.is_dir() else path
    out = []
    bad = 0

    def say(text):
        out.append(text)
        print(text)

    def check(ok, text):
        nonlocal bad
        say(("PASS " if ok else "FAIL ") + text)
        if not ok:
            bad += 1

    if not log.exists():
        say(f"FAIL no log at {log}")
        return 2
    lines = []
    for raw in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = LINE.match(raw)
        if m:
            lines.append(m.group("msg"))
    step = None
    calls, guest, sent, menu = {}, {}, {}, []
    widths = []
    levels = []
    gets = []
    started = False
    for msg in lines:
        m = re.match(r"dev pad: log VIB (\w+)", msg)
        if m:
            step = m.group(1)
            started = True
            calls.setdefault(step, [])
            guest.setdefault(step, [])
            sent.setdefault(step, [])
            continue
        m = re.match(r"vibration: driver call #\d+ left=(\d+) right=(\d+)", msg)
        if m and step:
            calls[step].append((int(m.group(1)), int(m.group(2))))
            continue
        m = re.match(r"vibration: game #\d+ user=(\d+) left=(\d+) right=(\d+): (.*?)(?: \((?:on|off)\))?$", msg)
        if m and step:
            text = m.group(4)
            decision = ("held back" if text.startswith("held back")
                        else "to the drivers" if text.startswith("to the drivers")
                        else "sent as a stop" if text.startswith("sent as a stop") else text)
            guest[step].append((int(m.group(2)), int(m.group(3)), decision))
            continue
        m = re.match(r"vibration: sent user=(\d+) left=(\d+) right=(\d+) \((switched on|switched off);", msg)
        if m and step:
            sent[step].append((int(m.group(2)), int(m.group(3)), m.group(4)))
            continue
        m = re.match(r"settings menu: (mcla_vibration|mcla_adaptive_triggers) = (\w+)", msg)
        if m and started:
            menu.append((step, m.group(1), m.group(2)))
            continue
        m = re.match(r"settings menu: '(\w+)' layout: menu.width (\d+) height (\d+) count (\d+)", msg)
        if m and started:
            widths.append((m.group(1), int(m.group(2)), int(m.group(4))))
            continue
        m = re.match(r"settings menu: '(\w+)' is (\d+) wide \(over (\d+)\): the texts go to level (\d+)", msg)
        if m and started:
            levels.append(msg)
            continue
        m = re.match(r"dev pad: get (.*)", msg)
        if m and started:
            gets.append((step, m.group(1).strip()))
    if not started:
        say("FAIL the log has no 'dev pad: log VIB' line: the script did not run (see timeout.png and the log's end)")
        return 1
    check("14" in calls, "the script reached its last step (VIB 14)")
    for s, want in CALLS.items():
        got = calls.get(s)
        if got is None:
            check(False, f"step {s}: not in the log")
            continue
        check(got == want, f"step {s}: driver calls {got} (expected {want})")
    for s, want in GUEST.items():
        got = guest.get(s, [])
        check(got == want, f"step {s}: the game's calls {got} (expected {want})")
    for s, want in SENT.items():
        got = sent.get(s, [])
        check(got == want, f"step {s}: sent by the update {got} (expected {want})")
    # With the option off, the game's own rumble reached no driver in these steps: 1, 2, 3, 7, 8, 10, 13, 14.
    off_calls = sum(len(calls.get(s, [])) for s in ("1", "2", "3", "7", "8", "10"))
    check(off_calls == 0, f"steps with the option off and the game rumbling (1, 2, 3, 7, 8, 10): {off_calls} driver calls in all (expected 0)")
    on_nonzero = [c for s in ("3b", "5", "11") for c in calls.get(s, []) if c != (0, 0)]
    check(len(on_nonzero) == 3, f"steps with the option on: {len(on_nonzero)} calls with a motor running (expected 3: the 3 rumbles)")

    # the pause menu's rows
    seq = []
    for (_, o, v) in menu:
        if not seq or seq[-1] != (o, v):  # one press can set an option more than once
            seq.append((o, v))
    want =[("mcla_vibration", "false"), ("mcla_vibration", "true"),
            ("mcla_adaptive_triggers", "false"), ("mcla_adaptive_triggers", "true")]
    check(seq == want, f"the pause menu's rows set {seq} (expected {want})")
    values = {}
    for step_, text in gets:
        for m in re.finditer(r"(mcla_vibration|mcla_adaptive_triggers)=(\w+)", text):
            values.setdefault(m.group(1), []).append((step_, m.group(2)))
    say(f"INFO get after each row press: {values}")
    vib = [v for _, v in values.get("mcla_vibration", [])]
    trig = [v for _, v in values.get("mcla_adaptive_triggers", [])]
    check(vib[:2] == ["false", "true"], f"mcla_vibration after the row's presses: {vib[:2]} (expected false, true)")
    check(trig[:2] == ["false", "true"], f"mcla_adaptive_triggers after the row's presses: {trig[:2]} (expected false, true)")
    controls = [w for w in widths if w[0] == "CONTROLS"]
    check(bool(controls), "the CONTROLS page's layout was logged")
    if controls:
        w = controls[-1]
        check(w[1] <= 750, f"CONTROLS page: menu.width {w[1]} with {w[2]} rows (cap 750)")
    check(not levels, f"no page went to a shorter wording by itself ({len(levels)} lines)")
    after = folder / "mcla.toml.after"
    if after.exists():
        text = after.read_text(encoding="utf-8", errors="replace")
        for key in ("mcla_vibration", "mcla_adaptive_triggers"):
            m = re.search(r"^\s*" + key + r"\s*=\s*(\w+)", text, re.M)
            check(bool(m) and m.group(1) == "true", f"the menu saved {key} = {m.group(1) if m else '(absent)'} in mcla.toml (expected true, the value last set)")
    else:
        say("INFO no mcla.toml.after in the folder: the saved file was not checked")
    say("RESULT " + ("all checks passed" if not bad else f"{bad} check(s) FAILED"))
    if a.summary:
        pathlib.Path(a.summary).write_text("\n".join(out) + "\n", encoding="utf-8")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

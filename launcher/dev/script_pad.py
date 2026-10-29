r"""Script pad check (fork, 2026-10-29): do the game's scripts miss button presses above 30 FPS outside the title screen too, and does
--mcla_script_pad_latch_ms with --mcla_script_pad_latch_scope=all tell them once?

  python -I script_pad.py pad <kind> <out.pad> [--route FILE] [--hold MS] [--presses N]
  python -I script_pad.py report <run folder>... [--out FILE]

Pad kinds (all end with `quit`; the dev pad, src/dev_pad.cpp, drives the game from the scratch save):
  city   boot, the title (press_until start left_title), the map zoom-out, free roam; then in jittered times (gaps of 1.0 to 1.4 s, so
         the press falls on every phase of the game's updates and of the script threads' rounds): `roam_buttons` N presses each of
         a, b, x, y, lb, rb, left, right, down; `pause` Start / B five times (the pause menu); `map` d-pad up twice, four times;
         `back_button` Back / B; `drive` a route followed (--route) with a press of x, y, a, b, lb, rb every 1.0 to 1.4 s.
  race   boot, the city, the pause menu, Modes, Arcade, the first ordered race with the arcade defaults; `race_intro`: presses of A
         while the race loads and the intro runs; `race`: from the race's start, presses of x, y, lb, rb, a, b; `race_pause`: Start / B.
Every run needs --mcla_pad_probe=true (the audit).

What the audit says (src/pad_probe.cpp, ScriptPoll and FinalizeAuditLocked): for every press of a button or an action, every script
call site (program and program counter of the thread, the native, the source) that had asked about it within 200 ms before the press
is audited for 0.6 s: `saw` = told once (the real answer, or the latched one), `MISSED` = never told although it asked after the press,
`gone` = never told and never asked again (a screen closed by the press, a script that ended), `DOUBLED` = told in two game updates
(it would act twice). With the latch off the figures are what the game does; with it on they are what the fix does.
"""
import argparse
import re
import sys
from collections import OrderedDict, defaultdict
from pathlib import Path

# ---- pads --------------------------------------------------------------------------------------------------------------------


def gaps(n, base=1000, spread=420, seed=1):
    """n gaps in ms in [base, base + spread), deterministic and all different: the presses fall on every phase of the updates."""
    out = []
    x = 7919 * seed
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append(base + (x >> 8) % spread)
    return out


START = ["wait_until frames 20 90s", "wait 4s", "mark title", "press_until start left_title every=1500ms hold=100ms timeout=120s",
         "mark map_zoom", "wait_until roam 180s"]


def city_lines(hold=70, presses=6, route=None):
    out = ["# Made by script_pad.py (city). Do not edit: change the generator."] + START
    out += ["mark free_roam", "wait 6s", "screens", "mark roam_buttons"]
    for i, b in enumerate(["a", "b", "x", "y", "lb", "rb", "left", "right", "down"]):
        for g in gaps(presses, seed=i + 1):
            out += [f"press {b} {hold}ms", f"wait {g}ms"]
    out += ["mark roam_buttons_end", "wait 1s", "mark pause"]
    for g in gaps(5, 1000, 400, seed=20):
        out += ["press start 90ms", "wait 1500ms", "screens", "press b 90ms", f"wait {g}ms", "screens"]
    out += ["mark map"]
    for g in gaps(4, 1000, 500, seed=30):
        out += ["press up 90ms", "wait 2s", "screens", "press up 90ms", f"wait {g}ms"]
    out += ["mark back_button", "press back 90ms", "wait 2s", "screens", "press b 90ms", "wait 1500ms", "screens"]
    out += ["mark drive"]
    if route:
        out += [f"follow {route}", "wait 3s"]
    buttons = ["x", "y", "a", "b", "lb", "rb"] * 3
    for b, g in zip(buttons, gaps(len(buttons), 1000, 400, seed=40)):
        out += [f"press {b} {hold}ms", f"wait {g}ms"]
    if route:
        out += ["follow status", "follow stop"]
    out += ["mark end", "wait 2s", "quit"]
    return out


def race_lines(hold=70):
    out = ["# Made by script_pad.py (race). Do not edit: change the generator."] + START
    out += ["mark menu", "wait 1s 60f", "press start 4f", "wait 1500ms 90f",
            "press rb 4f", "wait 1000ms 60f", "press a 4f", "wait 2500ms 150f", "press a 4f", "wait 1500ms 90f", "press a 4f", "wait 1500ms 90f",
            "press a 4f", "wait_until roam 120s", "mark race_intro", "autocontinue on", "trigger rt 1", "wait 3s"]
    for g in gaps(8, 1200, 700, seed=50):
        out += [f"press a {hold}ms", f"wait {g}ms"]
    out += ["wait_until f32 0x827D7510 >= 9.3 300s", "mark race", "wait 2s", "screens"]
    buttons = ["x", "y", "lb", "rb", "a", "b"] * 3
    for b, g in zip(buttons, gaps(len(buttons), 1000, 500, seed=60)):
        out += [f"press {b} {hold}ms", f"wait {g}ms"]
    out += ["mark race_pause"]
    for g in gaps(3, 1000, 400, seed=70):
        out += ["press start 90ms", "wait 1500ms", "screens", "press b 90ms", f"wait {g}ms", "screens"]
    out += ["mark end", "autocontinue off", "wait 1s", "quit"]
    return out


def pad_lines(kind, hold=70, presses=6, route=None):
    if kind == "city":
        return city_lines(hold, presses, route)
    if kind == "race":
        return race_lines(hold)
    raise SystemExit(f"unknown pad kind {kind!r}")


# ---- report ------------------------------------------------------------------------------------------------------------------
TS = re.compile(r"\] \[t\d+\] (TS .*)$")
MARK = re.compile(r"dev pad: mark (\w+) at engine frame (\d+), swap (\d+)")
AUDIT = re.compile(r"TS audit native=(\S+) program=([0-9A-F]{8}) pc=(\d+) src=(\S+) press_seq=(\d+) verdict=(\w+) told_updates=(\d+) "
                   r"first=(\S+) delay_us=(\d+) polled_after=(\d)")
SITENEW = re.compile(r"TS site-new native=(\S+) program=([0-9A-F]{8}) \(name hash ([0-9A-F]{8})\) pc=(\d+) src=(\S+) thread=([0-9A-F]{8}) "
                     r"t=(\d+) update=(\d+)")
LOAD = re.compile(r'TS script load name="([^"]*)" shown="([^"]*)" hash=([0-9A-F]{8}) t=(\d+) update=(\d+)')
SITES = re.compile(r"TS sites t=(\d+) n=(\d+) audits: eligible=(\d+) saw=(\d+) missed=(\d+) doubled=(\d+) gone=(\d+)(.*)$")
ROW = re.compile(r"(\w+) ([0-9A-F]{8}):(\d+) (\S+) r(\d+) e(\d+) l(\d+)")
SCREENS = re.compile(r"dev pad: screens -> (.*)$")
SCRIPT_SEC = re.compile(r"TS 1s-script t=(\d+) .* latch_ms=(\d+)")
PRESS = re.compile(r"press_until start left_title every=(\S+)(?: hold=(\S+))?.*-> (ok after ([\d.]+) s, (\d+) presses, frame=(\d+)|err .*)")
CAP = re.compile(r"frame timing: the frame-rate cap is now (\d+) FPS")


class Tally:
    def __init__(self):
        self.saw = self.missed = self.gone = self.doubled = 0
        self.latched_first = 0

    @property
    def eligible(self):
        return self.saw + self.missed + self.gone

    def add(self, verdict, first):
        if verdict == "DOUBLED":
            self.doubled += 1
            self.saw += 1
        elif verdict == "saw":
            self.saw += 1
        elif verdict == "MISSED":
            self.missed += 1
        else:
            self.gone += 1
        if first == "latched" and verdict in ("saw", "DOUBLED"):
            self.latched_first += 1

    def text(self):
        return (f"eligible {self.eligible}: saw {self.saw} (first told by the latch {self.latched_first}), MISSED {self.missed}, "
                f"gone {self.gone}, DOUBLED {self.doubled}")


def report_one(folder):
    folder = Path(folder)
    log = folder / "game.log"
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    names = {}  # name hash -> script name
    mark = "start"
    marks = []
    by_mark = OrderedDict()
    by_site = defaultdict(Tally)  # (native, program hash or program, pc, src) -> tally
    by_native = defaultdict(Tally)
    by_src_press = defaultdict(list)  # (mark, src, press_seq) -> verdicts of the call sites
    first_seen = OrderedDict()  # (native, program, pc, src) -> (hash, mark, update)
    rates = defaultdict(int)  # peak reads in a second
    screens = []
    latch_ms = None
    press = None
    caps = []
    formula = 0
    last_sites = None
    for line in text.splitlines():
        m = CAP.search(line)
        if m:
            caps.append(int(m.group(1)))
        m = PRESS.search(line)
        if m:
            press = (m.group(3), m.group(1), m.group(2))
        m = MARK.search(line)
        if m:
            mark = m.group(1)
            marks.append(mark)
            by_mark.setdefault(mark, Tally())
            continue
        m = SCREENS.search(line)
        if m:
            screens.append((mark, m.group(1)[:140]))
            continue
        m = TS.search(line)
        if not m:
            continue
        t = m.group(1)
        if t.startswith("TS record formula differs"):
            formula += 1
            continue
        m = LOAD.match(t)
        if m:
            names[m.group(3)] = m.group(1)
            continue
        m = SITENEW.match(t)
        if m:
            native, program, h, pc, src = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
            first_seen.setdefault((native, program, pc, src), (h, mark, int(m.group(8))))
            continue
        m = AUDIT.match(t)
        if m:
            native, program, pc, src, seq, verdict, told, first, delay, after = m.groups()
            by_mark.setdefault(mark, Tally()).add(verdict, first)
            by_site[(native, program, pc, src)].add(verdict, first)
            by_native[native].add(verdict, first)
            by_src_press[(mark, src, seq)].append(verdict)
            continue
        m = SITES.match(t)
        if m:
            last_sites = m
            for r in ROW.finditer(m.group(8)):
                key = (r.group(1), r.group(2), r.group(3), r.group(4))
                rates[key] = max(rates[key], int(r.group(5)))
            continue
        m = SCRIPT_SEC.match(t)
        if m:
            latch_ms = int(m.group(2))
    out = [f"=== {folder.name}"]
    out.append(f"latch {latch_ms} ms; frame-rate cap line(s): {caps[:3]}; title: {press[0] if press else 'no press_until line'} "
               f"(every {press[1] if press else '-'}, hold {press[2] if press else '-'})")
    out.append("marks: " + " ".join(marks))
    tot = Tally()
    for tl in by_native.values():
        tot.saw += tl.saw
        tot.missed += tl.missed
        tot.gone += tl.gone
        tot.doubled += tl.doubled
        tot.latched_first += tl.latched_first
    out.append("ALL (call site and press pairs): " + tot.text())
    out.append("per script native:")
    for n, tl in sorted(by_native.items()):
        out.append(f"  {n}: {tl.text()}")
    out.append("per context (the last mark before the press):")
    for k, tl in by_mark.items():
        if tl.eligible:
            out.append(f"  {k}: {tl.text()}")
    # per press: a press is SAW when every call site that was polling it was told
    press_total = press_saw = 0
    for (mk, src, seq), verdicts in by_src_press.items():
        press_total += 1
        if all(v in ("saw", "DOUBLED") for v in verdicts if v != "gone"):
            press_saw += 1
    out.append(f"presses with at least one polling call site: {press_total}; SAW by every call site that stayed {press_saw}, MISSED by at least one "
               f"{press_total - press_saw}")
    out.append("call sites (the script users): native program[name] pc src: reads a second at most, first seen in, audits")
    keys = sorted(set(first_seen) | set(by_site), key=lambda k: (k[0], k[3], k[1], int(k[2])))
    for k in keys:
        native, program, pc, src = k
        h, mk, upd = first_seen.get(k, ("?", "?", 0))
        name = names.get(h, "?")
        tl = by_site.get(k)
        out.append(f"  {native} {program}[{name}] pc {pc} {src}: <= {rates.get(k, 0)}/s, first in {mk}; " + (tl.text() if tl else "never audited (no press while it polled)"))
    if formula:
        out.append(f"RECORD FORMULA DIFFERS from the game's answer: {formula} line(s) (UILogic_IsPressed / Player_FlashingHighbeams)")
    if last_sites:
        out.append(f"last 1 s sites line: n={last_sites.group(2)} eligible={last_sites.group(3)} saw={last_sites.group(4)} missed={last_sites.group(5)} "
                   f"doubled={last_sites.group(6)} gone={last_sites.group(7)}")
    shown = [s for s in screens]
    if shown:
        out.append("screens (the UI manager's movies) after each check, first 14: " + " | ".join(f"{mk}: {s}" for mk, s in shown[:14]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pad")
    p.add_argument("kind")
    p.add_argument("out")
    p.add_argument("--route", default=None)
    p.add_argument("--hold", type=int, default=70)
    p.add_argument("--presses", type=int, default=6)
    r = sub.add_parser("report")
    r.add_argument("folders", nargs="+")
    r.add_argument("--out", default=None)
    a = ap.parse_args()
    if a.cmd == "pad":
        lines = pad_lines(a.kind, a.hold, a.presses, a.route)
        Path(a.out).write_text("\n".join(lines) + "\n", encoding="ascii")
        print(f"{a.out}: {len(lines)} lines")
    else:
        text = []
        for f in a.folders:
            text += report_one(f) + [""]
        s = "\n".join(text)
        if a.out:
            Path(a.out).write_text(s + "\n", encoding="utf-8")
        print(s)


if __name__ == "__main__":
    main()

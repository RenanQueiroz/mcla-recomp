r"""The title screen's Start presses (fork issue, owner's report 2026-10-29): pads for the dev pad, and a report on the
lines the probe (mcla_pad_probe, src/pad_probe.cpp) and the dev pad leave in a run's game.log.

  python -I title_start.py pad <kind> <out.pad> [--hold MS] [--every MS] [--settle S] [--idle S] [--shots DIR]
  python -I title_start.py report <run folder>... [--out FILE]

Pad kinds (all end with `quit`):
  repro    the pad of the earlier runs: wait 8 s, press_until start left_title every=2s (300 ms holds), the figures of the
           42 runs that always needed 9 presses (cap 30, 4 logical processors)
  press    wait for the engine (frames 20), settle S seconds, then press_until start left_title every=<every> hold=<hold>
  single   as press, but ONE press, then 20 s of nothing (shots at 0, 6, 12 and 20 s), then press_until: is a single
           press enough when the game then loads, or is it ignored?
  idle     sit at the title for <idle> seconds after engine frame 20 (shots every 30 s, then four more in the last
           part: the attract video starts about 120 s after launch), then 16 Start presses (every <every> ms) with a
           shot 700 ms after each: does the first press stop the video, and which press leaves the title?

`left_title` is NOT the press: the dev pad reads bit 0 of a function-local static's guard (0x82836A28) that the city's
lighting set-up sets the first time it runs, which can be some seconds after a press that was accepted. A press was
ACCEPTED when the trace shows `EVENT_START_SCREEN_EXIT handled` within 1.5 s of it (the title's statechart leaves
StartScreen on that event).

Report, per run (needs the probe: --mcla_pad_probe=true): the press_until result, per Start edge of the pad record the
update and the time after the first one, the way the key took (A modal scene, B1 main form and its child, B2 user
interface; with the vtable, its class when a table is given, and what it answered), the events posted, whether the exit
event came (and after how many updates), and how many movie ticks and input mapper runs the press update had; then the
counts of the 1 s lines (updates, UI updates, mapper runs, movie steps and ticks, "just pressed" native calls and how many
got past its modal check). A class table (address and class name per line; made from the image's RTTI, never committed)
is read from --vtables, the environment variable TS_VTABLES or C:\mcv\title2\tools\vtables.txt when it exists.
"""
import argparse
import re
import sys
from pathlib import Path

PEEKS = ["peeku 0x82836A28 1", "peeku 0x8288D000 1", "peeku 0x8286CF54 1"]  # city guard, game state, city pending


def pad_lines(kind, hold=100, every=1500, settle=4, idle=150, shots=None):
    out = [f"# Made by title_start.py ({kind}). Do not edit: change the generator."]

    def shot(name, scale=0.2):
        if shots:
            out.append(f"shot {shots}\\{name}.png {scale}")

    if kind == "repro":
        out += ["wait 8s", "mark title", "press_until start left_title every=2s timeout=90s", "mark city"]
        out += PEEKS + ["wait 1s", "quit"]
        return out
    out += ["wait_until frames 20 90s", f"wait {settle}s", "mark title"] + PEEKS
    if kind == "press":
        out += [f"press_until start left_title every={every}ms hold={hold}ms timeout=90s", "mark city"]
        out += PEEKS + ["wait 1s", "quit"]
    elif kind == "single":
        shot("t00")
        out += ["log TS single press", f"press start {hold}ms", "mark pressed"]
        shot("t01")
        out += ["wait 6s"] + PEEKS
        shot("t06")
        out += ["wait 6s"] + PEEKS
        shot("t12")
        out += ["wait 8s", "mark after20"] + PEEKS
        shot("t20")
        out += [f"press_until start left_title every=2s hold={hold}ms timeout=60s", "mark city"]
        out += PEEKS + ["wait 1s", "quit"]
    elif kind == "idle":
        # Sit at the title for <idle> seconds after engine frame 20 (the attract video starts about 120 s after launch),
        # shots on the way, then 16 Start presses (every <every> ms, <hold> ms) with a shot after each: a Start press
        # while the video plays stops it (or not), the next one leaves the title; the shots show which.
        left = float(idle)
        while left > 45:
            out.append("wait 30s")
            shot(f"idle_{int(idle - left + 30):03d}", 0.15)
            left -= 30
        quarter = max(1.0, left / 4.0)
        for i in range(4):
            out.append(f"wait {quarter:.1f}s")
            shot(f"idle_end{i + 1}", 0.15)
        out += ["mark idle_end"] + PEEKS
        for i in range(16):
            out += [f"press start {hold}ms", "wait 700ms"]
            shot(f"press{i + 1:02d}", 0.15)
            out.append(f"wait {max(100, every - 700 - hold)}ms")
        out += ["mark presses_end"] + PEEKS + ["wait 1s", "quit"]
    else:
        raise SystemExit(f"unknown pad kind {kind!r}")
    return out


# ---- report -----------------------------------------------------------------------------------------------------------------
import os

TS = re.compile(r"\] \[t\d+\] (TS .*)$")
STAMP = re.compile(r"^\[(\d{4}-\d\d-\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})\]")
PRESS = re.compile(r"press_until start left_title every=(\S+)(?: hold=(\S+))?.*-> (ok after ([\d.]+) s, (\d+) presses, frame=(\d+)|err .*)")
EDGE = re.compile(r"TS pad(\d) edge t=(\d+) update=(\d+) down=0x([0-9a-f]+) up=0x([0-9a-f]+)")
KEY = re.compile(r"TS menu key down input=(\d+) key=(-?\d+) taken=(\d+) t=(\d+) update=(\d+)")
SEC = re.compile(r"TS 1s t=(\d+) updates=(\d+) .*?ui_updates=(\d+) mapper=(\d+) movie_adv=(\d+) movie_ticks=(\d+) native_calls=(\d+) "
                 r"native_start=(\d+) native_pass=(\d+) events=(\d+) dispatch=(\d+) title_phase=(\w+)")
MARK = re.compile(r"dev pad: mark (\w+) at engine frame (\d+), swap (\d+)")
CAP = re.compile(r"frame timing: the frame-rate cap is now (\d+) FPS")
UPD = re.compile(r"update[= ](\d+)")
# script natives known from the code (sub_82554E20 calls them; the wrapper's address)
NATIVE_NAMES = {"822C8AB0": "IS_PAD_BUTTON_JUST_PRESSED", "822CC808": "MoviePlayer_StopMovie", "822CBEF8": "MoviePlayer_StartMovie",
                "822CCC00": "MoviePlayer_IsMoviePlaying?"}
HEX8 = re.compile(r"\b(?:vtable|vt) ([0-9A-F]{8})")


def secs(line):
    m = STAMP.match(line)
    if not m:
        return None
    return int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4)) + int(m.group(5)) / 1000.0


def load_vtables(path=None):
    path = path or os.environ.get("TS_VTABLES") or r"C:\mcv\title2\tools\vtables.txt"
    table = {}
    try:
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            parts = line.split(None, 1)
            if len(parts) == 2:
                table[parts[0].upper()] = parts[1].strip()
    except OSError:
        pass
    return table


def named(text, table):
    """Adds the class name after every `vtable XXXXXXXX` or `(vt XXXXXXXX)` when the table knows it."""
    def sub(m):
        name = table.get(m.group(1).upper())
        return m.group(0) + (f" <{name}>" if name else "")
    return HEX8.sub(sub, text)


def report_one(folder, vtables=None):
    table = vtables if vtables is not None else load_vtables()
    log = Path(folder) / "game.log"
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    lines = text.splitlines()
    out = [f"=== {Path(folder).name}"]
    press = None
    marks = {}
    edges, sec = [], []
    ts_lines = []  # (number of the last Start edge, or -1; the TS text)
    last_start = -1
    caps = []  # (seconds in the log, cap)
    first_edge_s = None
    for line in lines:
        m = CAP.search(line)
        if m:
            caps.append((secs(line), int(m.group(1))))
        m = PRESS.search(line)
        if m:
            press = (m.group(3), secs(line))
        m = MARK.search(line)
        if m:
            marks[m.group(1)] = (secs(line), int(m.group(2)))
        m = TS.search(line)
        if not m:
            continue
        t = m.group(1)
        m = EDGE.match(t)
        if m:
            e = (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4), 16), int(m.group(5), 16))
            edges.append(e)
            if e[0] == 0 and e[3] & 0x800:
                last_start += 1
                if first_edge_s is None:
                    first_edge_s = secs(line)
            continue
        m = SEC.match(t)
        if m:
            sec.append(tuple(int(m.group(i)) for i in range(1, 12)) + (m.group(12),))
            continue
        ts_lines.append((last_start, t))
    out.append(f"press_until: {press[0] if press else 'no result line (the pad did not get that far)'}")
    out.append("marks: " + ", ".join(f"{k}@frame {v[1]}" for k, v in marks.items()))
    if caps:
        ref = first_edge_s if first_edge_s is not None else caps[0][0]
        out.append("frame-rate cap changes (s after the first Start press): " + ", ".join(f"{c} FPS at {t - ref:+.1f}" for t, c in caps))
    start_edges = [e for e in edges if e[0] == 0 and e[3] & 0x800]
    accepted_n = 0
    for i, (pad, t, upd, down, up) in enumerate(start_edges):
        mine = [x for (j, x) in ts_lines if j == i]
        exit_line = next((x for x in mine if x.startswith("TS EVENT_START_SCREEN_EXIT")), None)
        exit_upd = None
        if exit_line:
            mu = UPD.search(exit_line)
            exit_upd = int(mu.group(1)) - upd if mu else None
            accepted_n += 1
        ways = [x for x in mine if x.startswith(("TS way", "TS main form", "TS child", "TS form", "TS focus"))]
        events = [x for x in mine if x.startswith("TS event posted")]
        ticks_here = sum(1 for x in mine if x.startswith("TS movie tick") and f"(update {upd})" in x)
        mapper_here = sum(1 for x in mine if x.startswith("TS mapper update") and f"(update {upd})" in x)
        movie = [x for x in mine if x.startswith("TS movie player")]
        natives = []
        for x in mine:
            if x.startswith("TS native "):
                fn = x.split()[2]
                if fn not in [n[0] for n in natives]:
                    natives.append([fn, 0])
                natives[[n[0] for n in natives].index(fn)][1] += 1
        out.append(f"press {i + 1:2d}: update {upd}, +{(t - start_edges[0][1]) / 1000.0:.0f} ms: "
                   + ("ACCEPTED (EVENT_START_SCREEN_EXIT" + (f" {exit_upd} updates later)" if exit_upd is not None else ")") if exit_line else "not accepted")
                   + f"; movie ticks in the press update {ticks_here}, mapper runs {mapper_here}, events posted {len(events)}")
        for x in mine:
            if x.startswith("TS state"):
                out.append("    " + named(x[3:], table))
        for x in ways:
            out.append("    " + named(x[3:], table))
        for x in events:
            out.append("    " + x[3:])
        for x in movie:
            out.append("    " + x[3:])
        if natives:
            out.append("    script natives called in the window (first 200 calls; function, count): " + ", ".join(f"{NATIVE_NAMES.get(fn, fn)} x{n}" for fn, n in natives))
    out.append(f"Start edges {len(start_edges)}; accepted {accepted_n}")
    keys = [KEY.match(x) for (_, x) in ts_lines if x.startswith("TS menu key down")]
    key_start = [k for k in keys if k and int(k.group(1)) == 7]
    out.append(f"menu key downs for Start {len(key_start)} (taken by the scene stack {sum(1 for k in key_start if int(k.group(3)))})")
    if sec:
        title = [s for s in sec if s[11] == "true"]
        med = lambda i: sorted(s[i] for s in sec)[len(sec) // 2]
        out.append(f"1 s lines: {len(sec)}; in the title phase {len(title)}; updates a second at the first {sec[0][1]}, median {med(1)}; "
                   f"a second (median): UI updates {med(2)}, mapper runs {med(3)}, movie steps {med(4)}, movie ticks {med(5)}, "
                   f"just-pressed native calls {med(6)} (for Start {med(7)}, past the modal check {med(8)}), events {med(9)}, key dispatches {med(10)}")
        end = next((i for i, s in enumerate(sec) if s[11] == "false"), None)
        out.append("title phase ended at the 1 s line number " + (str(end) if end is not None else "never") + f" of {len(sec)}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pad")
    p.add_argument("kind")
    p.add_argument("out")
    p.add_argument("--hold", type=int, default=100)
    p.add_argument("--every", type=int, default=1500)
    p.add_argument("--settle", type=float, default=4)
    p.add_argument("--idle", type=float, default=150)
    p.add_argument("--shots", default=None)
    r = sub.add_parser("report")
    r.add_argument("folders", nargs="+")
    r.add_argument("--out", default=None)
    r.add_argument("--vtables", default=None)
    a = ap.parse_args()
    if a.cmd == "pad":
        lines = pad_lines(a.kind, a.hold, a.every, a.settle, a.idle, a.shots)
        Path(a.out).write_text("\n".join(lines) + "\n", encoding="ascii")
        print(f"{a.out}: {len(lines)} lines")
    else:
        table = load_vtables(a.vtables)
        text = []
        for f in a.folders:
            text += report_one(f, table) + [""]
        s = "\n".join(text)
        if a.out:
            Path(a.out).write_text(s + "\n", encoding="utf-8")
        print(s)


if __name__ == "__main__":
    main()

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
lighting set-up sets the first time it runs, which can be some seconds after a press that was accepted. The report
therefore also gives, per Start edge of the pad record, whether a script read it (the `TS ... script read` lines) and
whether the menu's key event was taken by the scene stack (`TS menu key down ... taken=`).

Report, per run: the press_until result, the Start edges (time in ms after the first one, the update number), for each
whether a script read "just pressed" for button 11 (Start) in the same update or later (lag in updates), the
scripts' reads per second (the 1 s lines: updates, native calls, calls for Start), and when the title phase ended.
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
TS = re.compile(r"\] \[t\d+\] (TS .*)$")
STAMP = re.compile(r"^\[(\d{4}-\d\d-\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})\]")
PRESS = re.compile(r"press_until start left_title every=(\S+)(?: hold=(\S+))?.*-> (ok after ([\d.]+) s, (\d+) presses, frame=(\d+)|err .*)")
EDGE = re.compile(r"TS pad(\d) edge t=(\d+) update=(\d+) down=0x([0-9a-f]+) up=0x([0-9a-f]+)")
READ = re.compile(r"TS pad(\d) script read: button (\d+) just pressed( \(held over\))? t=(\d+) update=(\d+)")
KEY = re.compile(r"TS menu key down input=(\d+) key=(-?\d+) taken=(\d+) t=(\d+) update=(\d+)")
SEC = re.compile(r"TS 1s t=(\d+) updates=(\d+) .*?native_calls=(\d+) true=(\d+) start_calls=(\d+) start_true=(\d+) "
                 r"held_deliveries=(\d+) keys=(\d+) title_phase=(\w+)")
MARK = re.compile(r"dev pad: mark (\w+) at engine frame (\d+), swap (\d+)")


def secs(line):
    m = STAMP.match(line)
    if not m:
        return None
    return int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4)) + int(m.group(5)) / 1000.0


def report_one(folder):
    log = Path(folder) / "game.log"
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    lines = text.splitlines()
    out = [f"=== {Path(folder).name}"]
    press = None
    marks = {}
    edges, reads, keys, sec = [], [], [], []
    for line in lines:
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
            edges.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4), 16), int(m.group(5), 16)))
            continue
        m = READ.match(t)
        if m:
            reads.append((int(m.group(1)), int(m.group(2)), bool(m.group(3)), int(m.group(4)), int(m.group(5))))
            continue
        m = KEY.match(t)
        if m:
            keys.append(tuple(int(m.group(i)) for i in (1, 2, 3, 4, 5)))
            continue
        m = SEC.match(t)
        if m:
            sec.append(tuple(int(m.group(i)) for i in range(1, 9)) + (m.group(9),))
    out.append(f"press_until: {press[0] if press else 'no result line (the pad did not get that far)'}")
    out.append("marks: " + ", ".join(f"{k}@frame {v[1]}" for k, v in marks.items()))
    start_edges = [e for e in edges if e[0] == 0 and e[3] & 0x800]
    start_reads = [r for r in reads if r[0] == 0 and r[1] == 11]
    key_start = [k for k in keys if k[0] == 7]
    out.append(f"Start edges {len(start_edges)}; script reads of Start (true) {len(start_reads)} "
               f"(held over {sum(1 for r in start_reads if r[2])}); menu key downs for Start {len(key_start)} "
               f"(taken by the scene stack {sum(1 for k in key_start if k[2])})")
    seen = 0
    first_t = start_edges[0][1] if start_edges else 0
    for i, (pad, t, upd, down, up) in enumerate(start_edges, 1):
        r = [x for x in start_reads if x[4] >= upd and x[3] >= t and x[3] - t < 400_000]
        k = [x for x in key_start if x[4] >= upd and x[3] >= t and x[3] - t < 400_000]
        if r:
            seen += 1
        out.append(f"  press {i:2d}: +{(t - first_t) / 1000.0:8.1f} ms update {upd}  script read: "
                   + (f"yes, {r[0][4] - upd} updates / {(r[0][3] - t) / 1000.0:.1f} ms later" + (" (held over)" if r[0][2] else "") if r else "NO")
                   + "  menu key: " + (f"taken={k[0][2]}" if k else "none"))
    if start_edges:
        out.append(f"script saw {seen} of {len(start_edges)} Start presses ({100.0 * seen / len(start_edges):.0f} %)")
    if sec:
        title = [s for s in sec if s[8] == "true"]
        out.append(f"1 s lines: {len(sec)}; in the title phase {len(title)}; "
                   f"updates a second at the first {sec[0][1]}, median {sorted(s[1] for s in sec)[len(sec) // 2]}; "
                   f"script reads a second (median) {sorted(s[2] for s in sec)[len(sec) // 2]}, for Start {sorted(s[4] for s in sec)[len(sec) // 2]}")
        end = next((i for i, s in enumerate(sec) if s[8] == "false"), None)
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
    a = ap.parse_args()
    if a.cmd == "pad":
        lines = pad_lines(a.kind, a.hold, a.every, a.settle, a.idle, a.shots)
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

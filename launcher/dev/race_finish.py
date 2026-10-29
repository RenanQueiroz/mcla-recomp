"""The race finish check (fork-only dev tool, 2026-10-29): does anything in the sequence after the finish line run slower
than it should when the UI movies are paced?

LARecomp (https://github.com/mzzvxm/LARecomp, commit 7b9726e) reports that the 30 Hz pacing of the UI movies
(mcla_ui_timing_fix, on by default here too) makes the race finish screens, the podium and the results run in slow
motion. This tool writes the pad that drives a race to its end and reads the log of a run:

  race_finish.py pad <out.pad> --shots <dir> --route <file.route> --boot <boot-to-race.pad> [--finish-s 70]
      the pad: boot-to-race.pad, then the follower drives sunset-and-vine.route to its end, then the car keeps
      its throttle and the pad waits --finish-s seconds, a picture every 5 s (nothing is pressed after the route:
      what the game does by itself is what is measured)
  race_finish.py report <run folder>...
      one run: the movies the engine advanced (from the game.log lines of --mcla_ui_pace_log), how often, when each
      appeared and went (seconds after the finish's start), the engine timer's step and how fast its clock ran
  race_finish.py compare <reference run folder> <run folder>...
      each run against the reference (cap 30, the console's rate): per movie the time on screen, the game clock's
      speed, and a verdict. Slow motion = a movie that stays 15% and 1 s longer than in the reference, or a game
      clock that runs slower than the reference's.

Readings (all from the log):
  steps/s     advances of a movie per second (the engine asks `calls` times, `steps` of them went through)
  clock       seconds of the engine timer's clock (+20 of the timer object) per second of wall time
  step        the timer's own fixed step ([timer+32], believed 1/30 s on the console), the rate the movies were built for
  finish      the stretches in which a movie is on screen that begin more than FINISH_AFTER_S seconds after the race's
              start; the finish's start is the first of them, else the route's end. A movie that comes and goes counts
              each time, and the k-th stretch of a name is matched with the k-th of the same name in the other run.
Only seconds that the log has are used: a run that never reached the end of the route says so and gets what the log has.
"""

import argparse
import datetime as dt
import re
import statistics
import sys
from pathlib import Path

TS = re.compile(r"^\[(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})\]")
PACE = re.compile(r"ui pace: span_ms=([\d.]+) updates=(\d+) movies\(name:ctx:swf_fps:calls/steps/dtcalls\)(.*?) \| timer (.*)$")
KV = re.compile(r"(\w+)=(-?[\d.]+(?:e[-+]?\d+)?|nan|inf)", re.I)
ENTRY = re.compile(r"^(\S+):([0-9A-Fa-f]{8}):(\d+):(\d+)/(\d+)/(\d+)$")
MARK = re.compile(r"dev pad: mark (\w+) at engine frame (\d+)")
# A movie stretch that begins later than this many seconds after the race's start (the countdown ends about 9 s in, the pop-ups of the start come
# in the first 20 s, the route takes about 190 s) counts as part of the finish.
FINISH_AFTER_S = 100.0
# Seconds after the finish's start, for the clock readings.
WINDOWS = ((0, 10), (10, 20), (20, 40), (40, 80))


def stamp(line):
    m = TS.match(line)
    if not m:
        return None
    y, mo, d, h, mi, s, ms = (int(x) for x in m.groups())
    return dt.datetime(y, mo, d, h, mi, s, ms * 1000).timestamp()


def pad_lines(route, boot, shots, finish_s=70, every_s=5, scale=0.3, speed=1.0, start=True, finish=True, pictures=True,
              res=1, picture_every_ms=120, picture_count=100):
    """The pad. The shots folder must exist before the run (the game does not make it).

    start: the race start is measured too (race_start.py): the UI pace log in 100 ms windows and the cars' positions every
    3 ticks (50 ms of the mode timer) from the intro on, the throttle held through the countdown by the follower, and, with
    `pictures`, a picture of the whole frame about every picture_every_ms from 3 s of the mode timer (the countdown starts
    after it; the race starts at about 9.3) for picture_count pictures. Those pictures are scaled by 0.3 / res, so a 3x run
    makes the same size. `mark race` comes after them (at about 15 s of the mode timer; the finish reading counts from it, so it
    is a few seconds later than in a run without the start part).
    finish=False ends the script a few seconds after the start (a run for the start alone)."""
    lines = [
        "# The race start, and the race to its finish line and what follows (race_finish.py pad). Nothing is pressed after the route:",
        "# the follower hands the car back with the throttle open, and the pad waits and takes pictures.",
        f"include {boot}",
    ]
    if start:
        lines += [
            "# The start: fine log windows, every car's position (the player first), the throttle held from the intro on.",
            "set mcla_ui_pace_log_ms 100",
            "watch 3",
            f"follow {route} speed={speed}",
            "wait_until f32 0x827D7510 >= 3.0 120s",
            "mark countdown",
        ]
        if pictures:
            pic_scale = round(scale / max(1, res), 3)
            lines.append(f"burst {shots}\\cd {picture_count} {pic_scale} 0 0 1 1 every={picture_every_ms}")
        else:
            lines.append("wait_until f32 0x827D7510 >= 15 120s")
        lines += [
            "wait_until f32 0x827D7510 >= 9.3 120s",
            "mark race",
            "watch stop",
            "set mcla_ui_pace_log_ms 1000",
        ]
        if not finish:
            lines += ["wait 6s", "mark start_end"]
            return lines
    else:
        lines += [
            f"follow {route} speed={speed}",
            "wait_until f32 0x827D7510 >= 9.3 300s",
            "mark race",
        ]
    lines += [
        "wait_until follow_done 330s",
        "mark route_end",
        "trigger rt 1",
    ]
    for i in range(max(1, int(finish_s // every_s))):
        lines.append(f"shot {shots}\\f{i:02d}.png {scale}")
        lines.append(f"wait {every_s}s")
    lines.append("mark finish_end")
    return lines


class Run:
    """What one game.log says."""

    def __init__(self, folder):
        self.folder = Path(folder)
        self.tag = self.folder.name
        self.marks = {}
        self.samples = []
        self.route_end = None
        self.script_done = None
        self.args = ""
        args = self.folder / "args.txt"
        if args.exists():
            self.args = args.read_text(encoding="utf-8", errors="replace")
        log = self.folder / "game.log"
        self.text = log.read_text(encoding="utf-8", errors="replace").splitlines() if log.exists() else []
        for line in self.text:
            t = stamp(line)
            if t is None:
                continue
            m = PACE.search(line)
            if m:
                self.samples.append(self.sample(t, m))
                continue
            m = MARK.search(line)
            if m and m.group(1) not in self.marks:
                self.marks[m.group(1)] = t
            if "dev pad: follow reached the end of the route" in line and self.route_end is None:
                self.route_end = t
            if "dev pad: script " in line and " done: " in line:
                self.script_done = line.split(" done: ", 1)[1].strip()
        if self.route_end is None:
            self.route_end = self.marks.get("route_end")
        self.shots = len(list((self.folder / "shots").glob("*.png"))) if (self.folder / "shots").exists() else 0

    @staticmethod
    def sample(t, m):
        span = float(m.group(1)) / 1000.0
        movies = {}
        for entry in m.group(3).split():
            e = ENTRY.match(entry)
            if not e:
                continue
            name, ctx, swf, calls, steps, dtc = e.groups()
            old = movies.get(name)
            if old:
                old["calls"] += int(calls)
                old["steps"] += int(steps)
                old["dtc"] += int(dtc)
            else:
                movies[name] = dict(ctx=ctx, swf=int(swf), calls=int(calls), steps=int(steps), dtc=int(dtc))
        timer = {k: float(v) for k, v in KV.findall(m.group(4))}
        updates = int(m.group(2))
        return dict(t=t, span=span, updates=updates, fps=updates / span if span > 0 else 0.0, movies=movies, timer=timer)

    # ---- readings
    @property
    def cap(self):
        m = re.search(r"--mcla_fps=(-?\d+)", self.args)
        return int(m.group(1)) if m else None

    def window(self, t0, t1):
        return [s for s in self.samples if t0 <= s["t"] <= t1]

    def clock(self, t0, t1):
        """Engine clock seconds per wall second between two times (resets of the clock, at a mode change, are skipped)."""
        ss = self.window(t0, t1)
        gained = wall = 0.0
        for a, b in zip(ss, ss[1:]):
            d = b["timer"].get("total_t", 0.0) - a["timer"].get("total_t", 0.0)
            w = b["t"] - a["t"]
            if 0 < w < 5 and d >= 0:
                gained += d
                wall += w
        return gained / wall if wall > 0 else None

    def intervals(self, gap=2.5):
        """name -> the stretches in which the movie was asked about, one dict each (first, last, span, steps, calls, dtc, swf, n). A gap
        of more than `gap` seconds between two log lines that name it ends a stretch (a movie that goes and comes back is two)."""
        out, cur = {}, {}
        for s in self.samples:
            for name, m in s["movies"].items():
                c = cur.get(name)
                if c and s["t"] - c["last"] <= gap:
                    c["last"] = s["t"]
                    c["span"] += s["span"]
                    c["steps"] += m["steps"]
                    c["calls"] += m["calls"]
                    c["dtc"] += m["dtc"]
                    c["n"] += 1
                else:
                    c = dict(first=s["t"], last=s["t"], span=s["span"], steps=m["steps"], calls=m["calls"], dtc=m["dtc"], swf=m["swf"], n=1)
                    cur[name] = c
                    out.setdefault(name, []).append(c)
        for stretches in out.values():
            for c in stretches:
                c["steps_s"] = c["steps"] / c["span"] if c["span"] else 0.0
                c["calls_s"] = c["calls"] / c["span"] if c["span"] else 0.0
        return out

    def finish_movies(self):
        """The stretches that begin more than FINISH_AFTER_S s after the race's start: (name, ordinal among that name's finish stretches,
        stretch), by their start. Includes movies the race had before (a pop-up, a transition), because the finish reuses them."""
        t_race = self.marks.get("race")
        if t_race is None:
            return []
        found = []
        for name, stretches in self.intervals().items():
            k = 0
            for c in stretches:
                if c["first"] > t_race + FINISH_AFTER_S:
                    found.append((name, k, c))
                    k += 1
        return sorted(found, key=lambda x: x[2]["first"])

    def anchor(self):
        """The start of the finish: its first stretch, else the route's end, else None."""
        fin = self.finish_movies()
        if fin:
            return fin[0][2]["first"]
        return self.route_end


def f(x, digits=1):
    return "n/a" if x is None else f"{x:.{digits}f}"


def label(name, k):
    return name + ("#" + str(k + 1) if k else "")


def report_one(folder):
    r = Run(folder)
    out = [f"== {r.tag}: cap {r.cap}, script {r.script_done or 'did not report'}, {len(r.samples)} pace lines, {r.shots} pictures =="]
    if not r.samples:
        out.append("no 'ui pace' lines in game.log (was --mcla_ui_pace_log=true given, and does the exe have it?)")
        return out
    for name in ("race_intro", "race", "route_end", "finish_end"):
        if name in r.marks:
            out.append(f"mark {name} at {dt.datetime.fromtimestamp(r.marks[name]).strftime('%H:%M:%S')}")
    t_race = r.marks.get("race")
    last = r.samples[-1]["t"]
    if r.route_end is None:
        out.append("THE ROUTE DID NOT END (the car did not reach the end of the route within the pad's wait): the finish part is whatever the log has")
    elif t_race:
        out.append(f"race start to the route's end: {r.route_end - t_race:.1f} s")
    fin = r.finish_movies()
    t0 = r.anchor() or last
    out.append(f"the finish is taken to begin at the first movie that appears more than {FINISH_AFTER_S:.0f} s into the race: "
               + (f"{fin[0][0]}, {t0 - (r.route_end or t0):+.1f} s from the route's end" if fin else "none appeared; the route's end is used"))
    race = r.window((t_race or r.samples[0]["t"]) + 10, t0 - 5)
    after = [s for s in r.samples if s["t"] >= t0]
    if race:
        out.append(f"fps in the race: median {statistics.median(s['fps'] for s in race):.1f}, min {min(s['fps'] for s in race):.1f}")
    if after:
        out.append(f"fps from the finish's start: median {statistics.median(s['fps'] for s in after):.1f}, min {min(s['fps'] for s in after):.1f}, "
                   f"longest log window {max(s['span'] for s in after):.2f} s")
    out.append(f"the timer's step values seen (s): {sorted({round(s['timer'].get('step', 0.0), 5) for s in r.samples})}")
    out.append(f"timer time scale values seen: {sorted({round(s['timer'].get('scale', 0.0), 3) for s in r.samples})}")
    out.append(f"timer clock per wall second: race (the 40 s before the finish) {f(r.clock(t0 - 45, t0 - 5), 3)}"
               + "".join(f", {a}-{b} s after {f(r.clock(t0 + a, t0 + b), 3)}" for a, b in WINDOWS))
    out.append("movies in the race (steps/s, calls/s over the seconds they were asked about; swf = the file's own frame rate):")
    for name, stretches in sorted(r.intervals().items(), key=lambda kv: kv[1][0]["first"]):
        for c in stretches:
            if t_race and c["last"] >= t_race + 10 and c["first"] <= t_race + FINISH_AFTER_S:
                out.append(f"  {name:<22} steps/s {c['steps_s']:6.1f}  calls/s {c['calls_s']:6.1f}  elapsed-time-mode calls {c['dtc']:5d}  swf {c['swf']}  seen {c['n']} s")
    out.append(f"movies that begin in the finish ({len(fin)}); times are seconds after the finish's start:")
    for name, k, c in fin:
        gone = c["last"] < last - 1.5
        out.append(f"  {label(name, k):<22} from {c['first'] - t0:6.1f} to {c['last'] - t0:6.1f}  ({'gone' if gone else 'still there at the end'}, {c['last'] - c['first'] + 1:.0f} s)"
                   f"  steps/s {c['steps_s']:6.1f}  calls/s {c['calls_s']:6.1f}  swf {c['swf']}")
    if t_race:
        for name, stretches in sorted(r.intervals().items()):
            for c in stretches:
                if c["first"] <= t_race + FINISH_AFTER_S and t_race + FINISH_AFTER_S < c["last"] < last - 1.5:
                    out.append(f"  {name} (there in the race) goes away at {c['last'] - t0:+.1f} s")
    out.append("on screen from the finish's start (changes only; NAME=steps/asks in that second):")
    previous = None
    for s in after:
        names = tuple(sorted(s["movies"]))
        if names != previous:
            rates = " ".join(f"{n}={s['movies'][n]['steps']}/{s['movies'][n]['calls']}" for n in names)
            out.append(f"  +{s['t'] - t0:6.1f} s  fps {s['fps']:6.1f}  state {int(s['timer'].get('state', -1))}  {rates or '(no movie)'}")
            previous = names
    return out


def verdict(ref, run, tol=0.15, min_diff=1.0):
    """Lines and flags for a run against the reference."""
    lines = []
    flags = []
    fr = {(n, k): c for n, k, c in ref.finish_movies()}
    fn = {(n, k): c for n, k, c in run.finish_movies()}
    if not ref.samples or not run.samples or (not fr and ref.route_end is None) or (not fn and run.route_end is None):
        return [f"{run.tag}: cannot compare with {ref.tag} (no pace lines, or the route did not end and no movie began in the finish)"], ["no-finish"]
    last_ref = ref.samples[-1]["t"]
    last_run = run.samples[-1]["t"]
    a0, b0 = ref.anchor(), run.anchor()
    lines.append(f"-- {run.tag} (cap {run.cap}) against {ref.tag} (cap {ref.cap}) --")
    for key in sorted(set(fr) | set(fn), key=lambda k: (fr.get(k) or fn.get(k))["first"]):
        a, b = fr.get(key), fn.get(key)
        name = label(*key)
        if not a or not b:
            lines.append(f"  {name:<22} only in {'the reference' if a else run.tag}")
            continue
        da = a["last"] - a["first"] + 1
        db = b["last"] - b["first"] + 1
        cut_a = a["last"] >= last_ref - 1.5
        cut_b = b["last"] >= last_run - 1.5
        ratio = db / da if da else float("inf")
        note = "same speed"
        if cut_a or cut_b:
            note = "cut off by the end of the run in " + ("both" if cut_a and cut_b else "the reference" if cut_a else run.tag) + " (no ratio)"
            if cut_b and not cut_a and db > da + min_diff:
                flags.append(f"slower:{name}")
                note += ": SLOWER, still on screen after the reference's whole stay"
        elif ratio > 1 + tol and db - da > min_diff:
            note = "SLOWER"
            flags.append(f"slower:{name}")
        elif ratio < 1 - tol and da - db > min_diff:
            note = "faster"
            flags.append(f"faster:{name}")
        lines.append(f"  {name:<22} from +{b['first'] - b0:5.1f} s (reference +{a['first'] - a0:5.1f} s), on screen {db:5.0f} s (reference {da:5.0f} s), ratio {ratio:4.2f}, "
                     f"steps/s {b['steps_s']:5.1f} (reference {a['steps_s']:5.1f})  {note}")
    for lo, hi in WINDOWS:
        c_ref = ref.clock(a0 + lo, a0 + hi) if a0 else None
        c_run = run.clock(b0 + lo, b0 + hi) if b0 else None
        flag = ""
        if c_ref and c_run and c_run < c_ref * (1 - tol):
            flag = "  SLOWER CLOCK"
            flags.append(f"clock:{lo}-{hi}")
        lines.append(f"  timer clock per wall second, {lo}-{hi} s after the finish's start: {f(c_run, 3)} (reference {f(c_ref, 3)}){flag}")
    steps = sorted({round(s["timer"].get("step", 0.0), 5) for s in run.samples if b0 and s["t"] >= b0})
    if steps and any(abs(x - 1 / 30) > 0.0005 for x in steps if x > 0):
        flags.append("step-not-30")
        lines.append(f"  the timer's own step in the finish is not 1/30 s: {steps}  (movies built for another rate?)")
    lines.append(f"  verdict: {'; '.join(flags) if flags else 'nothing slower than the reference'}")
    return lines, flags


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("pad")
    a.add_argument("out")
    a.add_argument("--shots", required=True)
    a.add_argument("--route", required=True)
    a.add_argument("--boot", required=True)
    a.add_argument("--finish-s", type=int, default=70)
    a.add_argument("--speed", type=float, default=1.0)
    r = sub.add_parser("report")
    r.add_argument("folders", nargs="+")
    c = sub.add_parser("compare")
    c.add_argument("reference")
    c.add_argument("folders", nargs="+")
    o = p.parse_args()
    if o.cmd == "pad":
        Path(o.out).write_text("\n".join(pad_lines(o.route, o.boot, o.shots, o.finish_s, speed=o.speed)) + "\n", encoding="ascii")
        return 0
    if o.cmd == "report":
        for folder in o.folders:
            print("\n".join(report_one(folder)))
            print()
        return 0
    ref = Run(o.reference)
    for folder in o.folders:
        lines, _flags = verdict(ref, Run(folder))
        print("\n".join(lines))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

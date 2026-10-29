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
      appeared and went (seconds after the route's end), the engine timer's step and how fast its clock ran
  race_finish.py compare <reference run folder> <run folder>...
      each run against the reference (cap 30, the console's rate): per movie the time on screen, the game clock's
      speed, and a verdict. Slow motion = a movie that stays 15% and 1 s longer than in the reference, or a game
      clock that runs slower than the reference's.

Readings (all from the log):
  steps/s     advances of a movie per second (the engine asks `calls` times, `steps` of them went through)
  clock       seconds of the engine timer's clock (+20 of the timer object) per second of wall time
  step        the timer's own fixed step ([timer+32], 1/30 s on the console), the rate the movies were built for
Only seconds that the log has are used: a run that never reached the end of the route says so and gets the race part.
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
# Movies that are on screen for the whole race: not part of what the finish brings.
RACE_MOVIES = ("HUDGAUGE", "HUDMAPMOVIE")


def stamp(line):
    m = TS.match(line)
    if not m:
        return None
    y, mo, d, h, mi, s, ms = (int(x) for x in m.groups())
    return dt.datetime(y, mo, d, h, mi, s, ms * 1000).timestamp()


def pad_lines(route, boot, shots, finish_s=70, every_s=5, scale=0.3, speed=1.0):
    """The pad. The shots folder must exist before the run (the game does not make it)."""
    lines = [
        "# The race to its finish line and what follows (race_finish.py pad). Nothing is pressed after the route:",
        "# the follower hands the car back with the throttle open, and the pad waits and takes pictures.",
        f"include {boot}",
        f"follow {route} speed={speed}",
        "wait_until f32 0x827D7510 >= 9.3 300s",
        "mark race",
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

    def movie_stats(self, t0=None, t1=None):
        """name -> first seen, last seen, seconds with samples, mean steps/s and calls/s, swf fps, steps in all."""
        out = {}
        for s in self.samples:
            if (t0 is not None and s["t"] < t0) or (t1 is not None and s["t"] > t1):
                continue
            for name, m in s["movies"].items():
                o = out.setdefault(name, dict(first=s["t"], last=s["t"], span=0.0, steps=0, calls=0, dtc=0, swf=m["swf"], n=0))
                o["last"] = s["t"]
                o["span"] += s["span"]
                o["steps"] += m["steps"]
                o["calls"] += m["calls"]
                o["dtc"] += m["dtc"]
                o["n"] += 1
        for o in out.values():
            o["steps_s"] = o["steps"] / o["span"] if o["span"] else 0.0
            o["calls_s"] = o["calls"] / o["span"] if o["span"] else 0.0
        return out

    def finish_movies(self, lead=5.0):
        """Movies first seen from `lead` s before the route's end on (the finish brings them), with their stats."""
        if self.route_end is None:
            return {}
        stats = self.movie_stats()
        before = self.movie_stats(None, self.route_end - lead)
        return {n: o for n, o in stats.items() if n not in before and o["first"] >= self.route_end - lead}


def f(x, digits=1):
    return "n/a" if x is None else f"{x:.{digits}f}"


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
    if r.route_end is None:
        out.append("THE ROUTE DID NOT END (the car did not reach the end of the route within the pad's wait): no finish part")
        t_end = r.samples[-1]["t"]
    else:
        t_end = r.route_end
        if t_race:
            out.append(f"race start to the route's end: {t_end - t_race:.1f} s")
    race = r.window(t_race or r.samples[0]["t"], t_end - 20)
    after = [s for s in r.samples if s["t"] >= t_end]
    if race:
        out.append(f"fps in the race: median {statistics.median(s['fps'] for s in race):.1f}, min {min(s['fps'] for s in race):.1f}")
    if after:
        out.append(f"fps after the route's end: median {statistics.median(s['fps'] for s in after):.1f}, min {min(s['fps'] for s in after):.1f}, "
                   f"max span of one log window {max(s['span'] for s in after):.2f} s")
    steps = sorted({round(s["timer"].get("step", 0.0), 5) for s in r.samples})
    out.append(f"the timer's step values seen (s): {steps}")
    out.append(f"timer clock per wall second: race (the 40 s before the route's end) {f(r.clock(t_end - 40, t_end - 1), 3)}"
               + "".join(f", {a}-{b} s after {f(r.clock(t_end + a, t_end + b), 3)}" for a, b in ((0, 10), (10, 20), (20, 40), (40, 70))))
    scales = sorted({round(s["timer"].get("scale", 0.0), 3) for s in r.samples})
    out.append(f"timer time scale values seen: {scales}")
    out.append("movies in the race (steps/s, calls/s over the seconds they were asked about; swf = the file's own frame rate):")
    stats = r.movie_stats(None, t_end - 5)
    for name, o in sorted(stats.items(), key=lambda kv: kv[1]["first"]):
        out.append(f"  {name:<22} steps/s {o['steps_s']:6.1f}  calls/s {o['calls_s']:6.1f}  elapsed-time-mode calls {o['dtc']:5d}  swf {o['swf']}  seen {o['n']} s")
    fin = r.finish_movies()
    out.append(f"movies that first appear in the finish ({len(fin)}); times are seconds after the route's end:")
    for name, o in sorted(fin.items(), key=lambda kv: kv[1]["first"]):
        gone = o["last"] < r.samples[-1]["t"] - 1.5
        out.append(f"  {name:<22} from {o['first'] - t_end:6.1f} to {o['last'] - t_end:6.1f}  ({'gone' if gone else 'still there at the end'}, {o['last'] - o['first'] + 1:.0f} s)"
                   f"  steps/s {o['steps_s']:6.1f}  calls/s {o['calls_s']:6.1f}  swf {o['swf']}")
    # What was on screen second by second after the route's end, only where it changes.
    out.append("on screen after the route's end (changes only):")
    previous = None
    for s in after:
        names = tuple(sorted(n for n in s["movies"] if n not in RACE_MOVIES))
        if names != previous:
            rates = " ".join(f"{n}={s['movies'][n]['steps']}/{s['movies'][n]['calls']}" for n in names)
            out.append(f"  +{s['t'] - t_end:6.1f} s  fps {s['fps']:6.1f}  state {int(s['timer'].get('state', -1))}  {rates or '(only the HUD movies)'}")
            previous = names
    return out


def verdict(ref, run, tol=0.15, min_diff=1.0):
    """Lines and flags for a run against the reference."""
    lines = []
    flags = []
    if ref.route_end is None or run.route_end is None:
        return [f"{run.tag}: cannot compare, the route did not end in " + (ref.tag if ref.route_end is None else run.tag)], ["no-route-end"]
    last_ref = ref.samples[-1]["t"]
    last_run = run.samples[-1]["t"]
    fr, fn = ref.finish_movies(), run.finish_movies()
    lines.append(f"-- {run.tag} (cap {run.cap}) against {ref.tag} (cap {ref.cap}) --")
    for name in sorted(set(fr) | set(fn), key=lambda n: (fr.get(n) or fn.get(n))["first"]):
        a, b = fr.get(name), fn.get(name)
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
            if cut_b and not cut_a and db > da:
                note += ": still longer than the reference's whole stay"
        elif ratio > 1 + tol and db - da > min_diff:
            note = "SLOWER"
            flags.append(f"slower:{name}")
        elif ratio < 1 - tol and da - db > min_diff:
            note = "faster"
            flags.append(f"faster:{name}")
        lines.append(f"  {name:<22} on screen {db:5.0f} s (reference {da:5.0f} s), ratio {ratio:4.2f}, steps/s {b['steps_s']:5.1f} (reference {a['steps_s']:5.1f})  {note}")
    for a, b in ((0, 10), (10, 20), (20, 40), (40, 70)):
        c_ref = ref.clock(ref.route_end + a, ref.route_end + b)
        c_run = run.clock(run.route_end + a, run.route_end + b)
        flag = ""
        if c_ref and c_run and c_run < c_ref * (1 - tol):
            flag = "  SLOWER CLOCK"
            flags.append(f"clock:{a}-{b}")
        lines.append(f"  timer clock per wall second, {a}-{b} s after the route's end: {f(c_run, 3)} (reference {f(c_ref, 3)}){flag}")
    steps = sorted({round(s["timer"].get("step", 0.0), 5) for s in run.samples if s["t"] >= run.route_end})
    if steps and any(abs(x - 1 / 30) > 0.0005 for x in steps if x > 0):
        flags.append("step-not-30")
        lines.append(f"  the timer's own step after the route's end is not 1/30 s: {steps}  (movies built for another rate?)")
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

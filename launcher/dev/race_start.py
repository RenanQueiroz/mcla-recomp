"""The race start check (fork-only dev tool, 2026-10-29): does the countdown keep up with the moment the race really starts?

The owner sees the race start while the "2" (cap 120, HUD stepped 60 a second) or the "3" (Smooth UI, every movie 30 a
second) is still on screen. This reads what race_finish.py's pad leaves in a run's folder:

  game.log   the UI pace log in 100 ms windows around the start (`ui pace: ... us=`), the cars' positions every 3 ticks
             of the mode timer (`dev pad: watch tick= frame= us= | x y z | ...`, the player first), the pictures' times
             (`dev pad: burst cd k= ... us=`), the marks
  shots\\cd-<k>.png   a picture about every 120 ms from 1.5 s of the mode timer on (the whole frame, scaled)

  race_start.py report <run folder>...            one run
  race_start.py compare <reference> <run>...      each run against the reference (cap 30, the console's rate)
  race_start.py sheet <run folder> [out.png]      a contact sheet of the countdown's pictures, with their times

What is measured, all on the steady clock (microseconds, `us=`):
  START      when the cars really start: the player's car (and the AI cars, their median) first moves 0.5 m from where it
             stood and keeps going. The throttle is held through the countdown (the follower), so this is the moment the
             game lets the car go; the mode timer ([0x827D7510], also logged) at that moment is the game's own reading.
  MOVIES     for every UI movie that is stepped near the start: when it first and last appears relative to START, its
             steps, steps a second of wall time and of the game's clock (the engine timer's total time), its own frame
             rate (swf), and the share of the update calls that stepped it. A movie that comes and goes around the start
             is a countdown candidate (the countdown may also be a clip inside a movie that stays: then the pictures and
             the steps per game second say it).
  GAME CLOCK engine timer seconds per wall second and updates a second over the seconds before the start.
  LAG        against the reference (cap 30): a movie that is gone from the screen later after START than in the
             reference, or that steps fewer times per game second than in the reference over the 4 s before START.
             The owner's report is a countdown that is 1 to 2 s behind; LAG_S (0.4 s) is the threshold used here.
"""

import argparse
import colorsys
import re
import statistics
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import race_finish  # noqa: E402

WATCH = re.compile(r"dev pad: watch tick=(\d+) frame=(\d+) us=(\d+)((?: \|(?: -?[\d.]+){3})+)")
BURST = re.compile(r"dev pad: burst (\S+) k=(\d+) swap=(\d+)\.\.(\d+) t=[\d.]+ ms us=(\d+) size=(\d+)x(\d+)")
POS = re.compile(r"\| (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)")
LAG_S = 0.4          # a countdown that ends this much later after START than in the reference is a lag
RATE_TOL = 0.15      # steps per game second below the reference's by more than this share: slower
MOVE_M = 0.5         # metres from where the car stood for it to have started
WINDOW_BEFORE_S = 12.0
WINDOW_AFTER_S = 5.0
# The countdown ring (3 red, 2 orange, 1 yellow, GO green) in the middle of the frame: centre and radius in a 384 x 216 picture (the size the pad's
# pictures have at every resolution scale), the hue bands of the colours, and how many ring pixels a picture needs to count as showing a number.
RING_CENTER = (190, 85)
RING_R_IN, RING_R_OUT = 6, 21
RING_SEQUENCE = ("red", "orange", "yellow", "green")
RING_NUMBER = {"red": "3", "orange": "2", "yellow": "1", "green": "GO"}
RING_MIN_PIXELS = 60      # a picture shows the ring when this many pixels of the colour are in the annulus
GO_TOLERANCE_S = 0.25     # PASS: GO appears within this many seconds of START (the console's reference: GO at START, to within one picture)


def f(x, digits=2):
    return "n/a" if x is None else f"{x:.{digits}f}"


class Start:
    """What one run's log says about the race start."""

    def __init__(self, folder):
        self.run = race_finish.Run(folder)
        self.folder = Path(folder)
        self.tag = self.folder.name
        self.watch = []      # (us, tick, frame, [(x, y, z), ...])
        self.pictures = []   # (k, us, swap0, swap1, w, h)
        for line in self.run.text:
            m = WATCH.search(line)
            if m:
                cars = [tuple(float(v) for v in p) for p in POS.findall(m.group(4))]
                self.watch.append((int(m.group(3)), int(m.group(1)), int(m.group(2)), cars))
                continue
            m = BURST.search(line)
            if m and m.group(1).replace("\\", "/").rsplit("/", 1)[-1].startswith("cd"):
                self.pictures.append((int(m.group(2)), int(m.group(5)), int(m.group(3)), int(m.group(4)), int(m.group(6)), int(m.group(7))))
        # fine windows: those with `us=` in the timer part
        self.samples = [s for s in self.run.samples if s["timer"].get("us")]
        self.start_us, self.start_info = self.find_start()

    # ---- the cars
    def move_times(self):
        """For each car (the player first): the steady-clock time (us) it started moving, else None.

        The car stood somewhere (`rest`, moved to the current line when the car jumps more than 40 m: the cars are put on the
        grid during the intro). The first line that is 0.15 m from it starts a candidate; it is a start when the distance from `rest`
        keeps growing over the next 12 lines to at least 2 m (a bump of the suspension is not). The release time is then the fit of
        the first metres: distance = a * (t - t0)^2 / 2 makes sqrt(distance) a straight line in t, and t0 its zero."""
        if len(self.watch) < 16:
            return []
        count = min(len(w[3]) for w in self.watch)

        def dist(a, b):
            return ((a[0] - b[0]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5

        out = []
        n_lines = len(self.watch)
        for c in range(count):
            found = None
            rest = self.watch[0][3][c]
            j = 1
            while j < n_lines - 12:
                pj = self.watch[j][3][c]
                d = dist(pj, rest)
                if d > 40:
                    rest = pj
                elif d >= 0.15:
                    ds = [dist(self.watch[k][3][c], rest) for k in range(j, j + 13)]
                    if max(ds) >= 2.0 and all(y > x - 0.05 for x, y in zip(ds, ds[1:])):
                        pts = []
                        for k in range(j, min(n_lines, j + 60)):
                            dk = dist(self.watch[k][3][c], rest)
                            if dk > 8.0:
                                break
                            pts.append((self.watch[k][0], dk ** 0.5))
                        t0 = self.watch[j - 1][0]
                        if len(pts) >= 4:
                            m = len(pts)
                            mx = sum(p[0] for p in pts) / m
                            my = sum(p[1] for p in pts) / m
                            sxx = sum((p[0] - mx) ** 2 for p in pts)
                            slope = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx if sxx > 0 else 0.0
                            if slope > 0:
                                fit = mx - my / slope
                                if self.watch[j - 1][0] - 1.5e6 < fit < self.watch[j][0]:
                                    t0 = fit
                        found = int(t0)
                        break
                    # a bump: the car settles somewhere new
                    rest = pj
                j += 1
            out.append(found)
        return out

    def find_start(self):
        """(us of the player's start, a dict of readings) or (None, {})."""
        moves = self.move_times()
        if not moves or moves[0] is None:
            return None, {"player": None, "ai": [m for m in moves[1:] if m]}
        ai = sorted(m for m in moves[1:] if m)
        info = {"player": moves[0], "ai": ai, "ai_median": (ai[len(ai) // 2] if ai else None)}
        # the game's own reading: the mode timer at that line (tick = timer * 60)
        for us, tick, _frame, _cars in self.watch:
            if us >= moves[0]:
                info["mode_t"] = tick / 60.0
                break
        return moves[0], info

    # ---- the pace windows
    def window_samples(self, lo_us, hi_us):
        return [s for s in self.samples if lo_us <= s["timer"]["us"] <= hi_us]

    def movies(self):
        """name -> dict(first, last, steps, calls, swf, ctxs) over the fine windows near the start, times in seconds relative to START."""
        if self.start_us is None:
            return {}
        t0 = self.start_us
        out = {}
        for s in self.window_samples(t0 - WINDOW_BEFORE_S * 1e6, t0 + WINDOW_AFTER_S * 1e6):
            rel = (s["timer"]["us"] - t0) / 1e6
            for name, m in s["movies"].items():
                d = out.setdefault(name, dict(first=rel, last=rel, steps=0, calls=0, swf=m["swf"], windows=0, dtc=0))
                d["first"] = min(d["first"], rel - s["span"])
                d["last"] = max(d["last"], rel)
                d["steps"] += m["steps"]
                d["calls"] += m["calls"]
                d["dtc"] += m["dtc"]
                d["windows"] += 1
        return out

    def rates_before_start(self, seconds=4.0):
        """Per movie over the `seconds` before START: steps, calls, wall seconds, game seconds; plus the clock readings."""
        if self.start_us is None:
            return {}, {}
        t0 = self.start_us
        win = self.window_samples(t0 - seconds * 1e6, t0)
        if len(win) < 3:
            return {}, {}
        # every window's counts cover the time before its line, so the first line only marks where the sums start
        body = win[1:]
        wall = sum(s["span"] for s in body)
        game = win[-1]["timer"].get("mode_t", 0.0) - win[0]["timer"].get("mode_t", 0.0)
        updates = sum(s["updates"] for s in body)
        movies = {}
        for prev, s in zip(win, body):
            game_span = s["timer"].get("mode_t", 0.0) - prev["timer"].get("mode_t", 0.0)
            for name, m in s["movies"].items():
                d = movies.setdefault(name, dict(steps=0, calls=0, swf=m["swf"], wall=0.0, game=0.0))
                d["steps"] += m["steps"]
                d["calls"] += m["calls"]
                d["wall"] += s["span"]
                d["game"] += game_span if game_span > 0 else 0.0
        clocks = dict(wall=wall, game=game, updates=updates, fps=updates / wall if wall else None,
                      game_per_wall=(game / wall if wall else None), min_fps=min(s["fps"] for s in body),
                      first_mode_t=win[0]["timer"].get("mode_t"), last_mode_t=win[-1]["timer"].get("mode_t"))
        return movies, clocks


def report_one(folder):
    s = Start(folder)
    r = s.run
    out = [f"== {s.tag}: cap {r.cap}, script {r.script_done or 'did not report'}, {len(r.samples)} pace lines ({len(s.samples)} fine), "
           f"{len(s.watch)} watch lines, {len(s.pictures)} countdown pictures =="]
    if not s.watch:
        out.append("no `dev pad: watch` lines: the cars were not watched (was the pad race_finish.py's, and is the exe the branch's?)")
    if s.start_us is None:
        out.append("THE RACE START WAS NOT FOUND: the player's car did not move 0.5 m (and keep going) in the watch lines")
        return out
    t0 = s.start_us
    info = s.start_info
    ai = info.get("ai", [])
    out.append(f"START (player's car first moves): mode timer {f(info.get('mode_t'), 2)} s; "
               + (f"AI cars: {len(ai)} moved, the first {(ai[0] - t0) / 1e6:+.2f} s, the median {(info['ai_median'] - t0) / 1e6:+.2f} s from the player's"
                  if ai else "no AI car moved in the watch lines"))
    movies, clocks = s.rates_before_start(4.0)
    if clocks:
        out.append(f"the 4 s before START: {f(clocks['fps'], 1)} updates a second (lowest window {f(clocks['min_fps'], 1)}), "
                   f"the game clock ran {f(clocks['game_per_wall'], 3)} s per wall second (mode timer {f(clocks['first_mode_t'], 3)} to {f(clocks['last_mode_t'], 3)})")
    out.append("movies near the start (times in s from START; steps a second over the 4 s before START in wall time / in the game's seconds; swf = the file's frame rate):")
    for name, d in sorted(s.movies().items(), key=lambda kv: kv[1]["first"]):
        b = movies.get(name)
        if b and b["wall"] > 0.3 and b["game"] > 0.3:
            per_wall = b["steps"] / b["wall"]
            per_game = b["steps"] / b["game"]
            calls = f"{b['steps']} steps in {b['calls']} asks over {b['wall']:.1f} s"
            rate = f"steps/s {per_wall:6.1f} wall {per_game:6.1f} game, {calls}"
        else:
            rate = "not stepped in the 4 s before START"
        out.append(f"  {name:<22} first {d['first']:+7.2f}  last {d['last']:+7.2f}  steps {d['steps']:5d}  swf {d['swf']:3d}  {rate}")
    out.append("(a movie first seen at or after -12 s and last seen before +5 s comes and goes around the start: the countdown candidates)")
    for name, d in sorted(s.movies().items(), key=lambda kv: kv[1]["first"]):
        if d["first"] > -WINDOW_BEFORE_S + 0.3 and d["last"] < WINDOW_AFTER_S - 0.3 and d["steps"]:
            seconds = max(0.1, d["last"] - d["first"])
            out.append(f"COUNTDOWN CANDIDATE {name}: on screen from {d['first']:+.2f} s to {d['last']:+.2f} s around START ({seconds:.2f} s), "
                       f"{d['steps']} steps = {d['steps'] / seconds:.1f} a second on average, swf {d['swf']} frames a second")
    if s.pictures:
        times = [(k, (us - t0) / 1e6) for k, us, *_ in s.pictures]
        out.append(f"pictures: {len(times)}, from {times[0][1]:+.2f} s to {times[-1][1]:+.2f} s from START; the gaps between them: median "
                   f"{statistics.median(b[1] - a[1] for a, b in zip(times, times[1:])) * 1000:.0f} ms" if len(times) > 1 else "pictures: 1")
        out.append("  pictures in order: " + " ".join(f"{k}:{t:+.2f}" for k, t in times[:: max(1, len(times) // 24)]))
        out += ring_lines(s)
    feed = countdown_feed_line(s)
    if feed:
        out.append(feed)
    glide = glide_line(s)
    if glide:
        out.append(glide)
    return out


def verdict(ref, run):
    """Lines and flags for a run against the reference."""
    a, b = Start(ref) if not isinstance(ref, Start) else ref, Start(run) if not isinstance(run, Start) else run
    lines, flags = [], []
    lines.append(f"-- {b.tag} (cap {b.run.cap}) against {a.tag} (cap {a.run.cap}) --")
    if a.start_us is None or b.start_us is None:
        return lines + ["  the race start was not found in " + (a.tag if a.start_us is None else b.tag)], ["no-start"]
    ma, mb = a.movies(), b.movies()
    ra, ca = a.rates_before_start(4.0)
    rb, cb = b.rates_before_start(4.0)
    lines.append(f"  START: mode timer {f(a.start_info.get('mode_t'))} s in the reference, {f(b.start_info.get('mode_t'))} s here")
    if ca and cb:
        lines.append(f"  game clock per wall second before START: {f(cb['game_per_wall'], 3)} (reference {f(ca['game_per_wall'], 3)}); "
                     f"updates a second {f(cb['fps'], 1)} (reference {f(ca['fps'], 1)}), lowest window {f(cb['min_fps'], 1)}")
        if cb["fps"] and cb["fps"] < 30:
            flags.append("fps-under-30")
            lines.append("  FPS UNDER 30 before START: movies stepped once per update fall behind the game clock")
        if ca["game_per_wall"] and cb["game_per_wall"] and abs(cb["game_per_wall"] / ca["game_per_wall"] - 1) > 0.05:
            flags.append("game-clock-differs")
            lines.append("  the game clock does not run at the reference's speed")
    for name in sorted(set(ma) | set(mb), key=lambda n: (mb.get(n) or ma.get(n))["first"]):
        da, db = ma.get(name), mb.get(name)
        if not da or not db:
            lines.append(f"  {name:<22} only in {'the reference' if da else b.tag}")
            continue
        persistent_a = da["first"] <= -WINDOW_BEFORE_S + 0.3 and da["last"] >= WINDOW_AFTER_S - 0.3
        gone_later = db["last"] - da["last"]
        note = ""
        if not persistent_a and gone_later > LAG_S:
            note = f"  LAG {gone_later:.2f} s: gone {gone_later:.2f} s later after START than in the reference"
            flags.append(f"lag:{name}")
        elif not persistent_a and gone_later < -LAG_S:
            note = f"  early by {-gone_later:.2f} s"
        line = (f"  {name:<22} first {db['first']:+6.2f} (ref {da['first']:+6.2f})  last {db['last']:+6.2f} (ref {da['last']:+6.2f})  "
                f"steps {db['steps']:4d} (ref {da['steps']:4d})  swf {db['swf']} (ref {da['swf']}){note}")
        ba, bb = ra.get(name), rb.get(name)
        if ba and bb and ba["game"] > 0.3 and bb["game"] > 0.3:
            ga = ba["steps"] / ba["game"]
            gb = bb["steps"] / bb["game"]
            line += (f"\n      steps per game second while on screen in the 4 s before START: {gb:6.1f} (ref {ga:6.1f}), "
                     f"per wall second {bb['steps'] / bb['wall']:6.1f} (ref {ba['steps'] / ba['wall']:6.1f})")
            if ga > 3 and gb < ga * (1 - RATE_TOL):
                line += "  SLOWER than the reference per game second"
                flags.append(f"slower:{name}")
        lines.append(line)
    ra_ring, rb_ring = ring_summary(a), ring_summary(b)
    if ra_ring["pictures"] and rb_ring["pictures"]:
        go_a, go_b = ra_ring["onset"].get("green"), rb_ring["onset"].get("green")
        lines.append(f"  countdown ring: number on screen at START {rb_ring['at_start'] or 'unknown'} (reference {ra_ring['at_start'] or 'unknown'}); GO appears at "
                     f"{f(go_b, 2) if go_b is not None else 'never'} s from START (reference {f(go_a, 2) if go_a is not None else 'never'})")
        if go_b is None or abs(go_b) > GO_TOLERANCE_S or (ra_ring["at_start"] and rb_ring["at_start"] != ra_ring["at_start"]):
            flags.append("countdown-out-of-step")
            lines.append(f"  COUNTDOWN OUT OF STEP: GO is not at START (within {GO_TOLERANCE_S:.2f} s) or the number at START differs from the reference's")
        else:
            lines.append("  COUNTDOWN PASS: the ring shows GO at START as in the reference")
    lines.append(f"  verdict: {'; '.join(flags) if flags else 'the countdown keeps up (nothing later or slower than the reference)'}")
    return lines, flags


def candidates(ref, run):
    """The rules for --mcla_ui_movie_rates a follow-up run should try: every movie flagged slower per game second gets `g:R` (R
    the reference's steps per game second, rounded; stepped by the game's clock with catching up), every movie flagged late and
    not already named gets the same from the 4 s before START in the reference. Returns [(name, rule, why)]."""
    a, b = Start(ref), Start(run)
    out = []
    if a.start_us is None or b.start_us is None:
        return out
    ra, ca = a.rates_before_start(4.0)
    rb, cb = b.rates_before_start(4.0)
    if not (ca and cb):
        return out
    ma, mb = a.movies(), b.movies()
    seen = set()
    for name, bb in rb.items():
        ba = ra.get(name)
        if not ba or ba["game"] <= 0.3 or bb["game"] <= 0.3:
            continue
        ga = ba["steps"] / ba["game"]
        gb = bb["steps"] / bb["game"]
        late = name in ma and name in mb and mb[name]["last"] - ma[name]["last"] > LAG_S and not (ma[name]["first"] <= -WINDOW_BEFORE_S + 0.3)
        slower = ga > 3 and gb < ga * (1 - RATE_TOL)
        if (slower or late) and name not in seen and 1 <= round(ga) <= 240 and name != "HUDGAUGE":
            seen.add(name)
            out.append((name, f"g:{round(ga)}", f"{'late' if late else 'slower'}: {gb:.1f} steps per game second, reference {ga:.1f}"))
    return out


# ---- pictures: reading the pad's stored PNGs, a contact sheet

def read_png(path):
    """(width, height, rows of RGB bytes) from a non-interlaced 8-bit RGB PNG of any filter type (the dev pad writes filter 0)."""
    data = Path(path).read_bytes()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 3
    rows = []
    for y in range(h):
        line = raw[y * (stride + 1):(y + 1) * (stride + 1)]
        if line[0] != 0:
            raise ValueError("a filtered PNG row: not written by the dev pad")
        rows.append(line[1:])
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(kind, body):
        c = struct.pack(">I", len(body)) + kind + body
        return c + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def ring_colours(path):
    """Counts of ring-coloured pixels (red, orange, yellow, green) in the annulus of the countdown ring of one picture."""
    w, h, rows = read_png(path)
    sx, sy = w / 384.0, h / 216.0
    cx, cy = RING_CENTER
    counts = dict.fromkeys(RING_SEQUENCE, 0)
    for y in range(max(0, int((cy - RING_R_OUT - 1) * sy)), min(h, int((cy + RING_R_OUT + 1) * sy))):
        row = rows[y]
        dy = (y - cy * sy) / sy
        if dy < -3:  # the smoke above the ring is orange too
            continue
        for x in range(max(0, int((cx - RING_R_OUT - 1) * sx)), min(w, int((cx + RING_R_OUT + 1) * sx))):
            dx = (x - cx * sx) / sx
            r2 = dx * dx + dy * dy
            if r2 > RING_R_OUT * RING_R_OUT or r2 < RING_R_IN * RING_R_IN:
                continue
            hue, sat, val = colorsys.rgb_to_hsv(row[x * 3] / 255, row[x * 3 + 1] / 255, row[x * 3 + 2] / 255)
            hue *= 360
            if sat < 0.55 or val < 0.28:
                continue
            if hue < 14 or hue > 340:
                counts["red"] += 1
            elif hue < 38:
                counts["orange"] += 1
            elif hue < 75:
                counts["yellow"] += 1
            elif 90 < hue < 160:
                counts["green"] += 1
    return counts


def ring_series(s):
    """[(k, seconds from START, colour or None, counts)] for the countdown pictures of a Start, in time order."""
    out = []
    if s.start_us is None:
        return out
    for k, us, *_ in sorted(s.pictures, key=lambda p: p[1]):
        path = s.folder / "shots" / f"cd-{k}.png"
        if not path.exists():
            continue
        counts = ring_colours(path)
        colour = max(counts, key=counts.get)
        out.append((k, (us - s.start_us) / 1e6, colour if counts[colour] >= 20 else None, counts))
    return out


def ring_summary(s):
    """What the pictures say about the countdown: {'onset': {colour: seconds from START of its first picture}, 'last': {...},
    'at_start': the label of the picture nearest START (a number, GO, or 'none'), 'at_start_s': its time, 'pictures': n}. The numbers are taken
    in order from the first strong red (smoke, lights and the HUD's other elements are orange or yellow too before the countdown starts)."""
    series = ring_series(s)
    res = {"onset": {}, "last": {}, "at_start": None, "at_start_s": None, "pictures": len(series)}
    if not series:
        return res
    for _k, rel, colour, counts in series:
        if colour and counts[colour] >= RING_MIN_PIXELS:
            want = RING_SEQUENCE[len(res["onset"])] if len(res["onset"]) < len(RING_SEQUENCE) else None
            if colour in res["onset"]:
                res["last"][colour] = rel
            elif colour == want and (colour == "red" or rel - res["last"].get(RING_SEQUENCE[len(res["onset"]) - 1], rel) < 3.0):
                res["onset"][colour] = rel
                res["last"][colour] = rel
    # The number at START: GO if a picture within one picture gap (125 ms) of START shows it, else what the nearest picture shows.
    near = min(series, key=lambda p: abs(p[1]))
    if abs(near[1]) <= 0.2:
        go = [p for p in series if abs(p[1]) <= 0.125 and p[2] == "green" and p[3]["green"] >= 20]
        pic = go[0] if go else near
        res["at_start"] = RING_NUMBER.get(pic[2], "none") if pic[2] and pic[3][pic[2]] >= 20 else "none"
        res["at_start_s"] = pic[1]
    return res


def ring_lines(s):
    """The report lines for the countdown ring of a run."""
    res = ring_summary(s)
    if not res["pictures"]:
        return ["countdown ring: no pictures"]
    on = res["onset"]
    parts = [f"{RING_NUMBER[c]} from {on[c]:+.2f} s" for c in RING_SEQUENCE if c in on]
    lines = ["countdown ring (the colour of the ring in the pictures: red 3, orange 2, yellow 1, green GO; times from START): "
             + (", ".join(parts) if parts else "no number found")]
    if len(on) >= 2:
        gaps = [on[b] - on[a] for a, b in zip(RING_SEQUENCE, RING_SEQUENCE[1:]) if a in on and b in on]
        lines.append("  seconds each number stayed (3 to 2, 2 to 1, 1 to GO): " + ", ".join(f"{g:.2f}" for g in gaps) + "  (1.0 on the console)")
    lines.append(f"  NUMBER ON SCREEN AT START: {res['at_start'] or 'unknown'}" + (f" (picture at {res['at_start_s']:+.2f} s)" if res["at_start_s"] is not None else ""))
    return lines


GLIDE = re.compile(r"ui interp cost: option=(\w+) calls=(\d+) total_us=[\d.]+ per_call_ns=[\d.]+ worst_us=[\d.]+ gliding=(\d+) applied=(\d+)")


def glide_line(s):
    """Smooth UI's own check (the `ui interp cost` lines of --mcla_ui_interp_stats, once a second): the draws of elements that had a glide to show,
    and how many of them the hook applied, over the whole run."""
    calls = gliding = applied = lines = 0
    option = None
    for line in s.run.text:
        m = GLIDE.search(line)
        if m:
            option = m.group(1)
            calls += int(m.group(2))
            gliding += int(m.group(3))
            applied += int(m.group(4))
            lines += 1
    if not lines:
        return None
    return (f"Smooth UI check (mcla_ui_interp_stats, {lines} lines, option {option}): {calls} hook calls, {gliding} draws with a glide to show, "
            f"{applied} applied" + (f" = {100.0 * applied / gliding:.1f}%" if gliding else ""))


def countdown_feed_line(s):
    """The HUD movie's countdown clock feed (cdn, cdwrit, cdsteps, cdfed of the pace lines) over the 4 s before START: what the game wrote
    (its update times summed), what the movie's steps read summed, and their ratio (1.0: the clock keeps the game's time)."""
    if s.start_us is None:
        return None
    win = s.window_samples(s.start_us - 4e6, s.start_us)
    writ = sum(x["timer"].get("cdwrit", 0.0) for x in win[1:])
    fed = sum(x["timer"].get("cdfed", 0.0) for x in win[1:])
    steps = sum(x["timer"].get("cdsteps", 0.0) for x in win[1:])
    n = sum(x["timer"].get("cdn", 0.0) for x in win[1:])
    if not win or n <= 0:
        return "countdown clock feed: no cdn= in the pace lines (the exe has no mcla_ui_countdown_fix, or the HUD's update never ran)"
    return (f"countdown clock feed over the 4 s before START: the game wrote {n:.0f} times (sum {writ:.3f} s), the HUD movie's {steps:.0f} steps read "
            f"{fed:.3f} s in all = {fed / writ if writ else 0:.2f} of the game's time (1.00 = the clock keeps the game's time; steps per update {steps / n:.2f})")


def contact_sheet(folder, out_png, every_s=0.25, cols=5, thumb_w=320, span=(-6.0, 3.0)):
    """A sheet of the countdown pictures, about one per `every_s`, in time order from `span` seconds around START. Returns the index lines."""
    s = Start(folder)
    shots = Path(folder) / "shots"
    if s.start_us is None or not s.pictures:
        return ["no sheet: no start found or no pictures"]
    chosen, last = [], None
    for k, us, *_ in s.pictures:
        rel = (us - s.start_us) / 1e6
        if not (span[0] <= rel <= span[1]):
            continue
        if last is None or rel - last >= every_s - 0.03:
            chosen.append((k, rel))
            last = rel
    tiles = []
    for k, rel in chosen:
        p = shots / f"cd-{k}.png"
        if not p.exists():
            continue
        w, h, rows = read_png(p)
        step = max(1, w // thumb_w)
        tw, th = w // step, h // step
        tile = [b"".join(rows[y * step][x * 3 * step:x * 3 * step + 3] for x in range(tw)) for y in range(th)]
        # a bar along the top: its length is the time (white at START, red before, green after), so the sheet reads without text
        bar = int(tw * min(1.0, abs(rel) / 6.0))
        colour = b"\x30\xd0\x30" if rel >= 0 else b"\xd0\x30\x30"
        for y in range(min(4, th)):
            tile[y] = colour * bar + tile[y][bar * 3:]
        tiles.append((k, rel, tw, th, tile))
    if not tiles:
        return ["no sheet: the picture files are missing"]
    tw, th = tiles[0][2], tiles[0][3]
    rows_n = (len(tiles) + cols - 1) // cols
    sheet = [bytearray(b"\x20" * (tw * cols * 3)) for _ in range(th * rows_n)]
    for i, (_k, _rel, w, h, tile) in enumerate(tiles):
        cx, cy = (i % cols) * tw, (i // cols) * th
        for y in range(min(h, th)):
            sheet[cy + y][cx * 3:cx * 3 + min(w, tw) * 3] = tile[y][:min(w, tw) * 3]
    write_png(out_png, tw * cols, th * rows_n, sheet)
    index = [f"sheet {out_png}: {len(tiles)} pictures, {cols} a row, left to right and top to bottom; the bar on top is red before START and green after (its length is the time, 6 s = full)"]
    for i in range(0, len(tiles), cols):
        index.append("  row " + str(i // cols) + ": " + "  ".join(f"cd-{k}:{rel:+.2f}s" for k, rel, *_ in tiles[i:i + cols]))
    return index


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("report")
    a.add_argument("folders", nargs="+")
    c = sub.add_parser("compare")
    c.add_argument("reference")
    c.add_argument("folders", nargs="+")
    k = sub.add_parser("sheet")
    k.add_argument("folder")
    k.add_argument("out", nargs="?")
    o = p.parse_args()
    if o.cmd == "report":
        for folder in o.folders:
            print("\n".join(report_one(folder)))
            print()
    elif o.cmd == "compare":
        ref = Start(o.reference)
        for folder in o.folders:
            lines, _flags = verdict(ref, Start(folder))
            print("\n".join(lines))
            print()
    else:
        out = o.out or str(Path(o.folder) / "countdown-sheet.png")
        print("\n".join(contact_sheet(o.folder, out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Record a race route from the AI opponents (fork-only dev tool).

Polls a running game's race cars (the pad pipe's "cars" command) and writes
the path of the car that went furthest as a route for "follow" (src/dev_pad.cpp):
"x,z,speed" lines, a point every few metres, speed in m/s.

  route.py record <file.route> [--seconds 240]   from the race start (run
                                                 it before the countdown ends)
  route.py from-log <run folder> <file.route>    the same from a run whose script had
                                                 `watch 15` on (arcade-race.pad WATCH=15)
  route.py show <file.route>                     length, points, speeds

The game runs the race at its own pace while this records, so start it with a
frame cap (devrun.py run --fps 60 --pipe mcla-pad --keep-open).
"""

import argparse
import math
import sys
import time

import pad


def parse_cars(reply):
    # ok frame=N cars=K | ADDR x y z | ...
    parts = reply.split("|")
    frame = int(parts[0].split("frame=")[1].split()[0])
    cars = {}
    for part in parts[1:]:
        address, x, _, z = part.split()
        cars[address] = (float(x), float(z))
    return frame, cars


def record(a):
    pipe = pad.connect(a.pipe, 30)
    tracks = {}
    start = time.monotonic()
    still_since = None
    while time.monotonic() - start < a.seconds:
        now = time.monotonic()
        frame, cars = parse_cars(pad.send(pipe, "cars"))
        for i, (address, xz) in enumerate(cars.items()):
            if i == 0:
                continue  # the player
            track = tracks.setdefault(address, [])
            # A jump (the race restarting, a reset after a crash) starts over.
            if track and math.dist(track[-1][2], xz) > 30:
                track.clear()
            track.append((now, frame, xz))
        # Stop once every opponent has stood still for a few seconds (finished).
        moving = False
        for track in tracks.values():
            if len(track) > 1:
                (t0, _, p0), (t1, _, p1) = track[-2], track[-1]
                moving |= math.dist(p0, p1) / max(t1 - t0, 1e-3) > 2
        # (Before the start nobody moves either.)
        started = any(math.dist(t[0][2], t[-1][2]) > 200 for t in tracks.values())
        if started:
            still_since = None if moving else (still_since or now)
            if still_since and now - still_since > 5:
                break
        time.sleep(a.interval)

    write_route(a, tracks)


def length(track):
    return sum(math.dist(p[2], q[2]) for p, q in zip(track, track[1:]))


def write_route(a, tracks, heights=None):
    """tracks {car: [(time s, frame, (x, z))]}: the route of the car that went furthest.
    heights {(x, z): y} gives each point a fourth column, the ground's height there (follow's
    teleport puts a car back at it)."""
    heights = heights or {}
    if not tracks:
        raise SystemExit("no opponents: is a race on?")
    address, track = max(tracks.items(), key=lambda kv: length(kv[1]))
    # Drop the part after the leader stopped.
    while len(track) > 2 and math.dist(track[-1][2], track[-2][2]) < 0.05:
        track.pop()
    print(f"{len(tracks)} opponents; the furthest, {address}, went {length(track):.0f} m "
          f"in {track[-1][0] - track[0][0]:.0f} s")

    # Resample every --spacing metres, with the speed it had there.
    points = [(track[0][2], 0.0, heights.get(track[0][2]))]
    carry = 0.0
    for (t0, _, p0), (t1, _, p1) in zip(track, track[1:]):
        d = math.dist(p0, p1)
        if d < 1e-6:
            continue
        speed = d / max(t1 - t0, 1e-3)
        while carry + d >= a.spacing:
            u = (a.spacing - carry) / d
            p0 = (p0[0] + (p1[0] - p0[0]) * u, p0[1] + (p1[1] - p0[1]) * u)
            d = math.dist(p0, p1)
            carry = 0.0
            points.append((p0, speed, heights.get(p1)))
        carry += d
    # The leader's speed from the start: the first point gets the second's.
    points[0] = (points[0][0], points[1][1] if len(points) > 1 else 0.0, points[0][2])
    with open(a.file, "w", encoding="utf-8", newline="\n") as out:
        out.write(f"# route recorded from opponent {address}: {len(points)} points, "
                  f"{a.spacing} m apart; x,z,speed (m/s)[,height (m)]\n")
        for (x, z), speed, y in points:
            out.write(f"{x:.2f},{z:.2f},{speed:.1f}" + ("" if y is None else f",{y:.2f}") + "\n")
    print(f"{a.file}: {len(points)} points")


def from_log(a):
    """The same route from a run's log, where the script had `watch <every>` on: the cars' positions
    at every <every> updates of the race timer (devrun.watch), the player first. Under
    --mcla_fixed_dt=N the ticks are 1/N s (60 without it); a run's game.log or its folder."""
    import devrun
    watched = devrun.watch(a.log)
    if not watched:
        raise SystemExit("no 'watch' lines in the log: run the script with `watch 15` (or so)")
    tracks, heights = {}, {}
    ticks = sorted(watched)
    for n, tick in enumerate(ticks):
        for i, p in enumerate(watched[tick]):
            if i == 0:
                continue  # the player
            track = tracks.setdefault(i, [])
            here = (p[0], p[2])
            if track and math.dist(track[-1][2], here) > 30:
                # One reading far away that the next one undoes (every car at once, 600 m off and
                # back, seen in races 5 and 15) is a glitch of the log, not a reset: skipped. Taking
                # it for a reset cut the route's first part off, so the cars started where it
                # never went.
                after = [(q[0], q[2]) for m in ticks[n + 1:n + 3] for q in [watched[m][i]] if i < len(watched[m])]
                if any(math.dist(track[-1][2], b) <= 30 * (k + 2) for k, b in enumerate(after)):
                    continue
                # A jump that stays (a reset after a crash) starts over.
                track.clear()
            heights[here] = p[1]
            track.append((tick / a.fps, tick, here))
    write_route(a, tracks, heights)


def show(a):
    points = []
    for line in open(a.file, encoding="utf-8"):
        if line[:1] in "-0123456789":
            x, z, s = map(float, line.split(",")[:3])
            points.append(((x, z), s))
    total = sum(math.dist(p[0], q[0]) for p, q in zip(points, points[1:]))
    speeds = [s for _, s in points]
    print(f"{len(points)} points, {total:.0f} m, speed {min(speeds):.0f}-{max(speeds):.0f} m/s "
          f"(mean {sum(speeds) / len(speeds):.0f}); from {points[0][0]} to {points[-1][0]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("record")
    p.add_argument("file")
    p.add_argument("--seconds", type=float, default=240)
    p.add_argument("--interval", type=float, default=0.05)
    p.add_argument("--spacing", type=float, default=4.0)
    p.add_argument("--pipe", default="mcla-pad")
    p = sub.add_parser("from-log")
    p.add_argument("log", help="a run folder, or its game.log")
    p.add_argument("file")
    p.add_argument("--spacing", type=float, default=4.0)
    p.add_argument("--fps", type=float, default=60.0, help="the watch ticks per second (the fixed "
                   "time step of the run, 60 by default)")
    p = sub.add_parser("show")
    p.add_argument("file")
    a = parser.parse_args()
    {"record": record, "from-log": from_log, "show": show}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())

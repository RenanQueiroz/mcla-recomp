"""Record a race route from the AI opponents (fork-only dev tool).

Polls a running game's race cars (the pad pipe's "cars" command) and writes
the path of the car that went furthest as a route for "follow" (src/dev_pad.cpp):
"x,z,speed" lines, a point every few metres, speed in m/s.

  route.py record <file.route> [--seconds 240]   from the race start (run
                                                 it before the countdown ends)
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

    def length(track):
        return sum(math.dist(p[2], q[2]) for p, q in zip(track, track[1:]))

    if not tracks:
        raise SystemExit("no opponents: is a race on?")
    address, track = max(tracks.items(), key=lambda kv: length(kv[1]))
    # Drop the part after the leader stopped.
    while len(track) > 2 and math.dist(track[-1][2], track[-2][2]) < 0.05:
        track.pop()
    print(f"{len(tracks)} opponents; the furthest, {address}, went {length(track):.0f} m "
          f"in {track[-1][0] - track[0][0]:.0f} s")

    # Resample every --spacing metres, with the speed it had there.
    points = [(track[0][2], 0.0)]
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
            points.append((p0, speed))
        carry += d
    # The leader's speed from the start: the first point gets the second's.
    points[0] = (points[0][0], points[1][1] if len(points) > 1 else 0.0)
    with open(a.file, "w", encoding="utf-8", newline="\n") as out:
        out.write(f"# route recorded from opponent {address}: {len(points)} points, "
                  f"{a.spacing} m apart; x,z,speed (m/s)\n")
        for (x, z), speed in points:
            out.write(f"{x:.2f},{z:.2f},{speed:.1f}\n")
    print(f"{a.file}: {len(points)} points")


def show(a):
    points = []
    for line in open(a.file, encoding="utf-8"):
        if line[:1] in "-0123456789":
            x, z, s = map(float, line.split(","))
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
    p = sub.add_parser("show")
    p.add_argument("file")
    a = parser.parse_args()
    record(a) if a.cmd == "record" else show(a)


if __name__ == "__main__":
    sys.exit(main())

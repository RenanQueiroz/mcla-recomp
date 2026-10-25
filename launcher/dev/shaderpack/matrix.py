"""The coverage matrix: jobs files for coverage.py batch (fork-only development tool).

  python matrix.py <folder> [--routes <folder>] [--paths rtv,fbo] [--sub-paths ""]
                   [--hold-every 3] [--traffic-every 4] [--probe-frames 30000]

writes, into <folder> (paths in the files have forward slashes; coverage.py refuses any path outside
the dev root or the repository):
  probes.txt        one run per arcade race (D3D12): the player stands in the race while it is run
                    with `watch`, and the route of the leading opponent is written to the routes
                    folder (arcade-race.pad --route-out), which every other run drives. A probe whose
                    route is shorter than 2,000 m (the race stalled on a tip, the leader was held up)
                    fails, its route is kept aside as .rejected, and a rerun redoes it
  grow1-<path>.txt  for each of --paths (rtv, fbo): every race in variant V1, the player following
                    the race's route (at 0.85 of its speed, put back on it when knocked off) and
                    pressing the car's buttons
  grow2-<path>.txt  the same in variant V2
  traffic-<path>.txt  every --traffic-every'th race in V1 again with the traffic option on, to see what
                    traffic cars add beyond the opponents'
  hold-<path>.txt   every --hold-every'th race in a third variant (V3) that the growth runs did not
                    play: run with the old pack and again with the grown one (coverage.py batch
                    --pack <grown> --suffix=-after), it says how much of the pack carries over
                    to what it was not made from
  roam-<path>.txt   free roam: a long circle in the city while time of day and weather move on
  grow1-<path>.txt for --sub-paths (rov, fsi) too when asked: every other race, V1 (not by default:
                    nearly every GPU takes rtv on D3D12 and fbo on Vulkan)

The races of the arcade lists (Modes > Arcade > Ordered, Circuit, Landmark: the first of each is
Sunset and Vine), counted from the menu's screenshots (explore-lists-*.pad). The variants set the
race options (arcade-race.pad):
  V1  dynamic weather and time, 3 opponents, no traffic; the car's buttons (nitro, camera view,
      headlights, handbrake, weight transfer, horn), the overhead map and the special ability pressed
  V2  noon, clear, power ups on (and its warning), 4 opponents, no traffic; the same buttons
  V3  dusk, rain, 3 opponents, no traffic; the same buttons
Traffic is off in them: cars knocked about by traffic stall the follower, and what traffic cars add is
measured by the traffic-*.txt runs.
"""
import argparse
from pathlib import Path

# kind (the arcade's race type), its letter, how many races its list has
KINDS = [(0, "o", 18), (1, "c", 14), (2, "l", 6)]
BUTTONS = "--var TOUR=1 --var MAP=1 --var LBSPAM=8"
NO_TRAFFIC = "--var TRAFFIC=1"  # Traffic: presses of right from Normal: None
VARIANTS = {
    "v1": f"{NO_TRAFFIC} {BUTTONS}",
    "v2": f"--var TIME=3 --var WEATHER=1 --var POWER=1 --var OPP=1 {NO_TRAFFIC} {BUTTONS}",
    "v3": f"--var TIME=4 --var WEATHER=4 {NO_TRAFFIC} {BUTTONS}",
    "v1t": BUTTONS,  # V1 with the traffic option as it is (Normal)
}
BACKEND = {"rtv": "d3d12", "rov": "d3d12", "fbo": "vulkan", "fsi": "vulkan"}
MIN_ROUTE_M = 2000


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=Path)
    ap.add_argument("--routes", type=Path, help="where the probes write the routes (default: <folder>/routes)")
    ap.add_argument("--ordered", type=int, default=KINDS[0][2])
    ap.add_argument("--circuit", type=int, default=KINDS[1][2])
    ap.add_argument("--landmark", type=int, default=KINDS[2][2])
    ap.add_argument("--paths", default="rtv,fbo")
    ap.add_argument("--sub-paths", default="")
    ap.add_argument("--sub-every", type=int, default=2)
    ap.add_argument("--hold-every", type=int, default=3)
    ap.add_argument("--traffic-every", type=int, default=4)
    ap.add_argument("--probe-frames", type=int, default=30000)
    a = ap.parse_args()
    counts = {"o": a.ordered, "c": a.circuit, "l": a.landmark}
    routes = (a.routes or a.folder / "routes").resolve()
    a.folder.mkdir(parents=True, exist_ok=True)
    routes.mkdir(parents=True, exist_ok=True)
    races = [(kind, letter, i) for kind, letter, _ in KINDS for i in range(counts[letter])]

    def route_of(letter, i):
        return (routes / f"{letter}{i:02d}.route").as_posix()

    def job(path, tag, kind, i, variant, letter):
        return (f"--script arcade-race.pad --path {path} --tag {tag} --timeout 1500 --min-route-m {MIN_ROUTE_M} "
                f"--var KIND={kind} --var RACE={i} {VARIANTS[variant]} --var ROUTE={route_of(letter, i)}")

    probes = ["# the leading opponent's route of each race (D3D12; the player stands in it)"]
    for kind, letter, i in races:
        probes.append(f"--script arcade-race.pad --backend d3d12 --tag probe-{letter}{i:02d} --timeout 900 "
                      f"--min-route-m {MIN_ROUTE_M} --var KIND={kind} --var RACE={i} --var WATCH=15 "
                      f"--var RACEFRAMES={a.probe_frames} --route-out {route_of(letter, i)}")
    (a.folder / "probes.txt").write_text("\n".join(probes) + "\n")
    for path in [p for p in a.paths.split(",") if p]:
        for n, variant in ((1, "v1"), (2, "v2")):
            lines = [f"# growth runs, {path}, {variant}"]
            for kind, letter, i in races:
                lines.append(job(path, f"g-{letter}{i:02d}-{variant}-{path}", kind, i, variant, letter))
            (a.folder / f"grow{n}-{path}.txt").write_text("\n".join(lines) + "\n")
        lines = [f"# traffic on (V1 again): every {a.traffic_every}th race, {path}"]
        for n, (kind, letter, i) in enumerate(races):
            if n % a.traffic_every == 0:
                lines.append(job(path, f"t-{letter}{i:02d}-v1t-{path}", kind, i, "v1t", letter))
        (a.folder / f"traffic-{path}.txt").write_text("\n".join(lines) + "\n")
        lines = [f"# hold-out runs, {path}: every {a.hold_every}th race, V3"]
        for n, (kind, letter, i) in enumerate(races):
            if n % a.hold_every == 0:
                lines.append(job(path, f"h-{letter}{i:02d}-v3-{path}", kind, i, "v3", letter))
        (a.folder / f"hold-{path}.txt").write_text("\n".join(lines) + "\n")
        (a.folder / f"roam-{path}.txt").write_text(
            f"# free roam, {path}: 12 minutes of circling while time of day and weather move on\n"
            f"--script roam-soak.pad --path {path} --tag roam-{path} --timeout 1800 --var MINUTES=12\n")
    for path in [p for p in a.sub_paths.split(",") if p]:
        lines = [f"# growth runs, {path}: every {a.sub_every}th race, V1"]
        for n, (kind, letter, i) in enumerate(races):
            if n % a.sub_every == 0:
                lines.append(job(path, f"g-{letter}{i:02d}-v1-{path}", kind, i, "v1", letter))
        (a.folder / f"grow1-{path}.txt").write_text("\n".join(lines) + "\n")
    print(f"{len(races)} races; files in {a.folder}")


if __name__ == "__main__":
    main()

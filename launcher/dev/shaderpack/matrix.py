"""The coverage matrix: jobs files for coverage.py batch (fork-only development tool).

  python matrix.py <folder> [--routes <folder>] [--paths rtv,fbo] [--sub-paths rov,fsi]
                   [--sub-every 2] [--hold-every 3]

writes, into <folder>:
  probes.txt        one run per arcade race (D3D12): the player stands in the race while it is run
                    with `watch`, and the route of the leading opponent is written to the routes
                    folder (arcade-race.pad --route-out), which every other run drives
  grow-<path>.txt   for each of --paths (rtv, fbo): every race in two variants (V1, V2 below), the
                    player following the race's route and pressing the car's buttons; for each of
                    --sub-paths (rov, fsi: the paths a GPU takes only on Intel's D3D12, or when
                    asked for): every --sub-every'th race, V1
  hold-<path>.txt   every --hold-every'th race in a third variant (V3) that the growth runs did not
                    play: run with the old pack and again with the grown one (coverage.py batch
                    --pack <grown> --suffix -after), it says how much of the pack carries over
                    to what it was not made from
  roam-<path>.txt   free roam: a long circle in the city while time of day and weather move on

The races of the arcade lists (Modes > Arcade > Ordered, Circuit, Landmark: the first of each is
Sunset and Vine), counted from the menu's screenshots (explore-lists-*.pad). The variants set the
race options (arcade-race.pad):
  V1  dynamic weather and time, 3 opponents; the car's buttons (nitro, camera view, headlights,
      handbrake, weight transfer, horn), the overhead map and the special ability pressed
  V2  noon, clear, power ups on (and its warning), 4 opponents, high traffic; the same buttons
  V3  dusk, rain, 3 opponents; the same buttons
"""
import argparse
from pathlib import Path

# kind (the arcade's race type), its letter, how many races its list has
KINDS = [(0, "o", 18), (1, "c", 14), (2, "l", 6)]
BUTTONS = "--var TOUR=1 --var MAP=1 --var LBSPAM=8"
VARIANTS = {
    "v1": BUTTONS,
    "v2": f"--var TIME=3 --var WEATHER=1 --var POWER=1 --var OPP=1 --var TRAFFIC=1 {BUTTONS}",
    "v3": f"--var TIME=4 --var WEATHER=4 {BUTTONS}",
}
BACKEND = {"rtv": "d3d12", "rov": "d3d12", "fbo": "vulkan", "fsi": "vulkan"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=Path)
    ap.add_argument("--routes", type=Path, help="where the probes write the routes (default: <folder>\\routes)")
    ap.add_argument("--ordered", type=int, default=KINDS[0][2])
    ap.add_argument("--circuit", type=int, default=KINDS[1][2])
    ap.add_argument("--landmark", type=int, default=KINDS[2][2])
    ap.add_argument("--paths", default="rtv,fbo")
    ap.add_argument("--sub-paths", default="rov,fsi")
    ap.add_argument("--sub-every", type=int, default=2)
    ap.add_argument("--hold-every", type=int, default=3)
    ap.add_argument("--variants", default="v1,v2")
    a = ap.parse_args()
    counts = {"o": a.ordered, "c": a.circuit, "l": a.landmark}
    routes = (a.routes or a.folder / "routes").resolve()
    a.folder.mkdir(parents=True, exist_ok=True)
    routes.mkdir(parents=True, exist_ok=True)
    races = [(kind, letter, i) for kind, letter, _ in KINDS for i in range(counts[letter])]

    def job(path, tag, kind, i, variant, route):
        return (f"--script arcade-race.pad --path {path} --tag {tag} --timeout 900 --var KIND={kind} "
                f"--var RACE={i} {VARIANTS[variant]} --var ROUTE={route}")

    probes = ["# the leading opponent's route of each race (D3D12; the player stands in it)"]
    for kind, letter, i in races:
        probes.append(f"--script arcade-race.pad --backend d3d12 --tag probe-{letter}{i:02d} --timeout 600 "
                      f"--var KIND={kind} --var RACE={i} --var WATCH=15 --var RACEFRAMES=10800 "
                      f"--route-out {routes / f'{letter}{i:02d}.route'}")
    (a.folder / "probes.txt").write_text("\n".join(probes) + "\n")
    for path in a.paths.split(","):
        grow = [f"# growth runs, {path}"]
        for n, (kind, letter, i) in enumerate(races):
            for v in a.variants.split(","):
                grow.append(job(path, f"g-{letter}{i:02d}-{v}-{path}", kind, i, v, routes / f"{letter}{i:02d}.route"))
        (a.folder / f"grow-{path}.txt").write_text("\n".join(grow) + "\n")
    for path in [p for p in a.sub_paths.split(",") if p]:
        grow = [f"# growth runs, {path}: every {a.sub_every}th race, V1"]
        for n, (kind, letter, i) in enumerate(races):
            if n % a.sub_every == 0:
                grow.append(job(path, f"g-{letter}{i:02d}-v1-{path}", kind, i, "v1", routes / f"{letter}{i:02d}.route"))
        (a.folder / f"grow-{path}.txt").write_text("\n".join(grow) + "\n")
    for path in a.paths.split(","):
        hold = [f"# hold-out runs, {path}: every {a.hold_every}th race, V3"]
        for n, (kind, letter, i) in enumerate(races):
            if n % a.hold_every == 0:
                hold.append(job(path, f"h-{letter}{i:02d}-v3-{path}", kind, i, "v3", routes / f"{letter}{i:02d}.route"))
        (a.folder / f"hold-{path}.txt").write_text("\n".join(hold) + "\n")
        (a.folder / f"roam-{path}.txt").write_text(
            f"# free roam, {path}: 12 minutes of circling while time of day and weather move on\n"
            f"--script roam-soak.pad --path {path} --tag roam-{path} --timeout 1800 --var MINUTES=12\n")
    print(f"{len(races)} races; files in {a.folder}")


if __name__ == "__main__":
    main()

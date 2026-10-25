"""Shader pack coverage runs (fork-only development tool, Windows).

Plays a controller script on a new, empty shader storage with the shader pack imported at the
start (as a player's storage is after the launcher's step), and reports what the pack did not
cover: the pipelines and shaders the game created while playing ("compiled in play": each was a
draw left out while it compiled), and the frames over 50 ms.

  python coverage.py soak  --script boot-to-roam.pad --backend d3d12 [--path rtv|rov|fbo|fsi]
                           --out <folder> --tag <name> [--pack <file>] [--no-pack] [--res 1]
                           [--fixed-dt 60] [--timeout 900] [--salt N] [--var NAME=VALUE ...]
                           [--route-out <file>] [--extra ...]
  python coverage.py batch <jobs file> --out <folder> [--lock] [--max-min 40] [--pack <file>]
                           [--suffix <text>]      (--pack and --suffix: the same jobs again, with
                           another pack and tags that don't collide)
  python coverage.py report <folder>... [--match <regex>]
  python coverage.py grow  <folder>... --out <pack> [--check] [--previous <pack>] [--drop <file>]
                           the pack plus what the soaks in the folders created in play (through
                           build_pack.py, which refuses a pack that lacks entries of the previous
                           one for no stated reason)
  matrix.py writes the jobs files of the races (see there); a script may be a template: see
  script_copy. Routes: a probe run (arcade-race.pad --var WATCH=15) with --route-out writes the
  leading opponent's route, which later runs drive (--var ROUTE=<file>).

A soak writes <out>/<tag>.json (and <tag>.new.txt: the descriptions created in play) and keeps its
storage in <out>/<tag>/cache (the folder build_pack.py reads). Runs go through devrun.py: private
save copy and cache root under MCLA_DEV_ROOT (the real data is checked before and after every
run), and MCLA_GAME_DATA for a checkout whose game\\ holds only default.xex. "batch" runs the lines
of a jobs file one after the other, each under the shared GPU lock (lockrun.py, which the
coordinating session provides: the path in COVERAGE_LOCKRUN), skipping jobs whose result exists.
A jobs file line is the arguments of "soak" (without --out), blank lines and # comments ignored.
"""
import argparse
import json
import math
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
DEVRUN = HERE.parent / "devrun.py"
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import build_pack as bp  # noqa: E402
import devrun  # noqa: E402  (load_segments)

TITLE = "545407F8"
# list file -> (record size, which render target path it is, the API)
LISTS = {
    "rtv.d3d12.xpso": (72, "rtv", "d3d12"),
    "rov.d3d12.xpso": (72, "rov", "d3d12"),
    "fbo.vk.xpso": (68, "fbo", "vulkan"),
    "fsi.vk.xpso": (68, "fsi", "vulkan"),
}
PATH_CVAR = {"rtv": "--render_target_path_d3d12=rtv", "rov": "--render_target_path_d3d12=rov",
             "fbo": "--render_target_path_vulkan=fbo", "fsi": "--render_target_path_vulkan=fsi"}
PATH_BACKEND = {"rtv": "d3d12", "rov": "d3d12", "fbo": "vulkan", "fsi": "vulkan"}
DEFAULT_PACK = REPO / "shaderpack" / f"{TITLE}.shaderpack"


# --- paths ---------------------------------------------------------------------------------------
# A path from a jobs file, a template variable or the command line is checked before anything is
# written or read through it: output only inside the dev root (MCLA_DEV_ROOT; the repository's
# shaderpack folder too for a pack), input only inside the dev root or this repository, and always
# absolute. A relative path would land in whatever folder the batch was started from (on 2026-10-25
# a jobs file's backslashes were eaten by the parser, "C:\\mcv\\shp-dev\\x" became "C:mcvshp-devx", which is
# relative to the current directory of drive C, and 28 route files were written into the owner's
# install folder).
DEV_ROOT = devrun.DEV


def lost_backslashes(text):
    return bool(re.match(r"^[A-Za-z]:[^/" + chr(92) * 2 + "]", str(text)))


def _inside(path, roots):
    resolved = Path(path).resolve()
    for root in roots:
        try:
            resolved.relative_to(Path(root).resolve())
            return True
        except ValueError:
            pass
    return False


def checked_path(value, what, write=False, allow_repo_pack=False):
    """value as an absolute Path, or SystemExit saying why not."""
    text = str(value)
    if lost_backslashes(text):
        raise SystemExit(f"{what}: {text!r} looks like a Windows path that lost its backslashes "
                         "(use forward slashes in jobs files)")
    path = Path(text)
    if not path.is_absolute():
        raise SystemExit(f"{what}: {text!r} is not an absolute path")
    roots = [DEV_ROOT] if write else [DEV_ROOT, REPO]
    if write and allow_repo_pack:
        roots.append(REPO / "shaderpack")
    if not _inside(path, roots):
        raise SystemExit(f"{what}: {text!r} is outside {' and '.join(str(r) for r in roots)}")
    return path


def checked_args(a):
    """Every path a soak takes, before it starts."""
    a.out = str(checked_path(a.out, "--out", write=True))
    if a.route_out:
        a.route_out = str(checked_path(a.route_out, "--route-out", write=True))
    if a.pack:
        a.pack = str(checked_path(a.pack, "--pack"))
    script = Path(a.script)
    if script.is_absolute() or lost_backslashes(a.script):
        checked_path(a.script, "--script")
    for item in a.var:
        name, _, value = item.partition("=")
        if re.match(r"^[A-Za-z]:", value) or "/" in value or chr(92) in value:
            if name == "ROUTE" and value in ("none", "0", ""):
                continue
            checked_path(value, f"--var {name}")
    for item in a.extra or []:
        _, _, value = item.partition("=")
        if re.match(r"^[A-Za-z]:", value):
            checked_path(value, f"--extra {item.split('=')[0]}")


# --- storages ------------------------------------------------------------------------------------
def read_storage(folder):
    """{'shaders': {hash: (pixel?, ucode)}, 'lists': {file name: [record bytes]}} of a shareable
    folder, each file up to its first record that does not match its hash (as the SDK loads it)."""
    folder = Path(folder)
    out = {"shaders": {}, "lists": {}}
    x = folder / f"{TITLE}.xsh"
    if x.exists():
        out["shaders"] = bp.read_shaders(x)
    for name, (size, _, _) in LISTS.items():
        f = folder / f"{TITLE}.{name}"
        out["lists"][name] = []
        if f.exists():
            d = f.read_bytes()
            if len(d) < 12 or d[:4] != b"XEPS":
                continue
            out["lists"][name] = _records(d, size)
    return out


def _records(d, size):
    # size is the storage record size (72 or 68): an XXH3 of the rest, then the description.
    out = []
    for p in range(12, len(d) - size + 1, size):
        r = d[p:p + size]
        if bp.h64(r[8:]) != struct.unpack_from("<Q", r)[0]:
            break
        out.append(r)
    return out


def record_key(r):
    return struct.unpack_from("<Q", r, 0)[0]


def record_shaders(r):
    """(vertex shader hash, pixel shader hash or 0) of a storage record."""
    return struct.unpack_from("<Q", r, 8)[0], struct.unpack_from("<Q", r, 24)[0]


def pack_content(pack_path):
    """{'shaders': {hash}, 'lists': {file name: {description hash}}} of a shader pack."""
    pk = bp.read_pack(Path(pack_path))
    return {"hash": bp.h64(Path(pack_path).read_bytes()),
            "shaders": {s["hash"] for s in pk["shaders"]},
            "lists": {n: {record_key(r) for r in recs} for n, (h, s, recs) in pk["lists"].items()}}


def created_in_play(storage, pack):
    """What a run's storage holds that the pack did not: new shaders, and per list the records
    (with how many of them use a shader the pack lacks)."""
    new_shaders = {h: v for h, v in storage["shaders"].items() if h not in pack["shaders"]}
    lists = {}
    for name, recs in storage["lists"].items():
        known = pack["lists"].get(name, set())
        new = [r for r in recs if record_key(r) not in known]
        lists[name] = new
    return new_shaders, lists


# --- one soak ------------------------------------------------------------------------------------
def frame_stats(folder):
    segs = devrun.load_segments(folder / "frames.csv")
    allf, per = [], {}
    for name, frames, _ in segs:
        if not frames:
            continue
        per[name] = {"frames": len(frames), "over50": sum(1 for f in frames if f > 50),
                     "over100": sum(1 for f in frames if f > 100), "worst_ms": round(max(frames), 1)}
        allf += frames
    total = {"frames": len(allf), "over50": sum(1 for f in allf if f > 50),
             "over100": sum(1 for f in allf if f > 100),
             "worst_ms": round(max(allf), 1) if allf else 0,
             "time_over50_s": round(sum(f for f in allf if f > 50) / 1000, 2),
             "seconds": round(sum(allf) / 1000, 1)}
    return total, per


def script_copy(script, shots, dest, variables=None):
    """The script as a run plays it, written to dest:
    - {SHOTS} is the screenshots folder, and the files the script names (include, follow, play) are
      by their absolute paths;
    - "#@default NAME=VALUE" gives a variable's value when --var NAME=VALUE doesn't, and {NAME}
      is replaced by it everywhere;
    - "@repeat N: command ; command" (the colon and a space end N) is N copies of the commands (N may be {NAME}; {I} in them is the
      copy's number, from 1);
      "@if V: command ; command" the commands once, unless V is empty, 0 or none;
    - {REMAINING}: RACEFRAMES less the frames the waits and presses since "mark race" took (a
      race of RACEFRAMES frames in all, however many buttons were pressed in it).
    Only coverage.py expands these: the plain dev pad takes a template for comments."""
    values = {}
    text = script.read_text(encoding="utf-8")
    for line in text.splitlines():
        if line.startswith("#@default "):
            name, _, value = line[len("#@default "):].partition("=")
            values[name.strip()] = value.split("#")[0].strip()
    values.update(variables or {})
    values["SHOTS"] = str(shots)
    expanded = []
    for line in text.splitlines():
        for name, value in values.items():
            line = line.replace("{" + name + "}", value)
        if line.startswith("@if "):
            condition, _, body = line[len("@if "):].partition(": ")
            if condition.strip() not in ("", "0", "none"):
                expanded += [c.strip() for c in body.split(";")]
        elif line.startswith("@repeat "):
            count, _, body = line[len("@repeat "):].partition(": ")
            expanded += [c.strip().replace("{I}", str(i)) for i in range(1, int(count) + 1) for c in body.split(";")]
        else:
            expanded.append(line)
    lines = []
    race_frames = int(values.get("RACEFRAMES", "0") or 0)
    spent = None  # frames of waits and presses since "mark race"
    for line in expanded:
        if line.strip() == "mark race":
            spent = 0
        if "{REMAINING}" in line:
            line = line.replace("{REMAINING}", str(max(0, race_frames - (spent or 0))))
        elif spent is not None:
            m = re.fullmatch(r"(?:wait|press [a-z]+) ([0-9]+)f", line.strip())
            spent += int(m.group(1)) if m else 0
        word = line.split(None, 1)
        if len(word) == 2 and word[0] in ("include", "follow", "play"):
            first, _, tail = word[1].partition(" ")
            if not Path(first).is_absolute() and (script.parent / first).exists():
                line = f"{word[0]} {(script.parent / first).resolve()}" + (f" {tail}" if tail else "")
        lines.append(line)
    left = [m for line in lines for m in re.findall(r"\{[A-Z_]+\}", line)]
    if left:
        raise SystemExit(f"{script.name}: no value for {sorted(set(left))} (--var NAME=VALUE)")
    dest.write_text("\n".join(lines) + "\n", encoding="utf-8")


def read_route(path):
    pts = []
    for line in Path(path).read_text().splitlines():
        if line[:1] in "-0123456789":
            x, z, v = map(float, line.split(",")[:3])
            pts.append((x, z, v))
    return pts


def route_length(path):
    pts = read_route(path)
    return sum(math.dist(p[:2], q[:2]) for p, q in zip(pts, pts[1:]))


def route_frames(path, fps=60, speed=1.0):
    """Frames a route takes to drive at `speed` times its own (a quarter more, and 10 s)."""
    pts = read_route(path)
    seconds = sum(math.dist(p[:2], q[:2]) / max(q[2] * speed, 3.0) for p, q in zip(pts, pts[1:]))
    return int(seconds * fps * 1.25) + 600


TMPSS_FRAME = re.compile(r"TMPSS (d3d12|vk) frame (.*)")


def visible_cost(log):
    """What a player would see of the pipelines compiled in play, from the dev build's per-frame
    counters (shader-stutter-timers.sdk.patch, TMPSS: a line for every frame that left draws out,
    waited, or did pipeline work on the GPU command thread):
      skipped  draws left out while their pipeline compiled (objects missing: pop-in), their total,
               the frames they were in, and the most in one frame;
      rescued  draws whose pipeline was made on the spot (an occlusion query's draw, a pipeline an
               earlier draw had queued): the frame waited for them;
      waited   the frame's wait for pipelines within the 4 ms budget, in ms: total and largest;
      endwait  the wait at the end of a submission (the other kind of wait), total ms;
      sync     pipeline work on the GPU command thread (depth-only pipelines and shader translation
               on D3D12, translation on Vulkan), total ms, the largest frame, frames over 4 ms;
      nopresent  Vulkan: presents skipped because the frame was incomplete (held up to 100 ms)."""
    out = {"frames_with_skips": 0, "skipped": 0, "max_skipped": 0, "rescued": 0, "waited_ms": 0.0,
           "max_waited_ms": 0.0, "endwait_ms": 0.0, "sync_ms": 0.0, "max_sync_ms": 0.0,
           "frames_sync_over_4ms": 0, "nopresent": 0, "lines": 0}
    for api, text in TMPSS_FRAME.findall(log):
        f = {k: float(v) for k, v in re.findall(r"(\w+)=([0-9.]+)", text)}
        out["lines"] += 1
        skipped = int(f.get("skipped", 0))
        out["frames_with_skips"] += skipped > 0
        out["skipped"] += skipped
        out["max_skipped"] = max(out["max_skipped"], skipped)
        out["rescued"] += int(f.get("rescued", 0))
        out["waited_ms"] += f.get("waited", 0.0)
        out["max_waited_ms"] = max(out["max_waited_ms"], f.get("waited", 0.0))
        out["endwait_ms"] += f.get("endwait", 0.0)
        out["sync_ms"] += f.get("sync", 0.0)
        out["max_sync_ms"] = max(out["max_sync_ms"], f.get("sync", 0.0))
        out["frames_sync_over_4ms"] += f.get("sync", 0.0) > 4.0
        out["nopresent"] += int(f.get("nopresent", 0))
    return {k: (round(v, 2) if isinstance(v, float) else v) for k, v in out.items()}


FOLLOW_LINE = re.compile(r"dev pad: follow (reached the end of the route|stopped): (.*)")


def follow_result(log):
    """The follower's last summary in a run's log: how far it got and what it took."""
    found = FOLLOW_LINE.findall(log)
    if not found:
        return None
    end, text = found[-1]
    fields = {k: v for k, v in re.findall(r"(\w+)=(\S+)", text)}
    best, total = (int(x) for x in fields.get("best", "0/1").split("/"))
    return {"end": end.startswith("reached"), "best": best, "total": total,
            "share": round(best / max(total, 1), 3),
            "recoveries": int(fields.get("recoveries", 0)), "teleports": int(fields.get("teleports", 0)),
            "retargets": int(fields.get("retargets", 0)), "popups": int(fields.get("popups", 0))}


def soak(a):
    checked_args(a)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    result_path = out / f"{a.tag}.json"
    cache = out / a.tag / "cache"
    if cache.parent.exists():
        shutil.rmtree(cache.parent)
    cache.mkdir(parents=True)
    backend = PATH_BACKEND[a.path] if a.path else a.backend
    script = Path(a.script)
    if not script.is_absolute() and not script.exists():
        script = HERE.parent / "scripts" / script
    shots = out / a.tag / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    run_script = out / a.tag / script.name
    variables = dict(v.split("=", 1) for v in a.var)
    route = variables.get("ROUTE", "none")
    auto_length = False
    if route not in ("none", "0", ""):
        found = Path(route) if Path(route).is_absolute() else script.parent / route
        if a.min_route_m and found.exists() and route_length(found) < a.min_route_m:
            raise SystemExit(f"{route}: only {route_length(found):.0f} m long (< {a.min_route_m} m): the probe that made "
                             "it stalled; make it again")
        if "RACEFRAMES" not in variables and found.exists():
            auto_length = True
            variables["RACEFRAMES"] = str(route_frames(found, a.fixed_dt or 60, float(variables.get("SPEED", "0.85"))))
    script_copy(script, shots, run_script, variables)
    cmd = [sys.executable, str(DEVRUN), "run", "--script", str(run_script), "--backend", backend,
           "--res", str(a.res), "--cache-root", str(cache), "--timeout", str(a.timeout)]
    if a.fixed_dt:
        cmd += ["--fixed-dt", str(a.fixed_dt)]
    if a.fps:
        cmd += ["--fps", str(a.fps)]
    if a.vsync is not None:
        cmd += ["--vsync", a.vsync]
    cmd += ["--intro", "fast"]
    if a.track:
        cmd.append("--track")  # the car's position twice a second in the run's pad.csv
    extra = list(a.extra or [])
    if a.path:
        extra.append(PATH_CVAR[a.path])
    pack = Path(a.pack) if a.pack else DEFAULT_PACK
    if a.no_pack:
        extra.append("--mcla_shader_pack_auto=false")
    else:
        extra.append(f"--mcla_shader_pack={pack.resolve()}")
    if a.salt:
        extra.append(f"--tmp_shader_salt={a.salt}")
    cmd += ["--extra"] + extra
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    text = proc.stdout + proc.stderr
    (out / f"{a.tag}.devrun.log").write_text(text, encoding="utf-8")
    result = {"tag": a.tag, "script": script.name, "backend": backend, "path": a.path, "res": a.res,
              "fixed_dt": a.fixed_dt, "pack": None if a.no_pack else str(pack),
              "wall_s": round(time.time() - t0, 1), "cmd": cmd}
    m = re.search(r"^(\d{8}-\d{6}\S*): exit (-?\d+)", text, re.M)
    if "REAL DATA CHANGED" in text:
        result["error"] = "REAL DATA CHANGED"
    elif "SCRIPT FAILED" in text:
        result["error"] = re.search(r"SCRIPT FAILED.*", text).group(0)
    if not m:
        result.setdefault("error", "devrun did not finish: " + text[-400:])
        result_path.write_text(json.dumps(result, indent=1))
        print(json.dumps({k: v for k, v in result.items() if k != "cmd"}))
        return 1
    folder = devrun.RUNS / m.group(1)
    result.update(run=m.group(1), exit=int(m.group(2)), run_folder=str(folder))
    if a.route_out:
        # The route of the car that went furthest, from the run's `watch` lines.
        import route as route_tool
        try:
            route_tool.from_log(argparse.Namespace(log=str(folder), file=str(a.route_out), spacing=4.0,
                                                   fps=float(a.fixed_dt or 60)))
            result["route"] = str(a.route_out)
            result["route_m"] = round(route_length(a.route_out))
            if a.min_route_m and result["route_m"] < a.min_route_m:
                # A route that stopped early (a tip's dialog stopped the race, or the leader was held
                # up) would make every run that drives it worthless: it is kept aside, not used.
                os.replace(a.route_out, str(a.route_out) + ".rejected")
                result["error"] = (f"stalled: the leader's route is only {result['route_m']} m (< {a.min_route_m} m); "
                                   f"kept as {Path(a.route_out).name}.rejected; the race's last picture: "
                                   f"{out / a.tag / 'shots' / 'race-end.png'}")
        except SystemExit as e:
            result["error"] = f"route: {e}"
    gpu = devrun.gpu_report(folder)
    result["gpu_clean"] = None if not gpu else gpu.get("clean")
    log = (folder / "game.log").read_text(encoding="utf-8", errors="replace") if (folder / "game.log").exists() else ""
    pre = re.search(r"Created (\d+) graphics pipelines", log)
    result["preloaded_at_start"] = int(pre.group(1)) if pre else 0
    m2 = re.search(r"shader pack: (\d+) shaders and (\d+) pipelines added", log)
    if m2:
        result["pack_added"] = [int(m2.group(1)), int(m2.group(2))]
    if (folder / "frames.csv").exists():
        result["frames"], result["segments"] = frame_stats(folder)
    result["visible"] = visible_cost(log)
    tips = re.findall(r"A pressed \((\d+) so far\)", log)
    result["tips_dismissed"] = int(tips[-1]) if tips else 0
    if route not in ("none", "0", ""):
        follow = follow_result(log)
        result["follow"] = follow
        if "error" not in result:
            if follow is None:
                result["error"] = "the follower reported nothing (the script ended before it?)"
            elif auto_length and not follow["end"] and follow["share"] < a.min_share:
                result["error"] = (f"stalled: the route was covered {follow['share']:.0%} (< {a.min_share:.0%}) with "
                                   f"{follow['recoveries']} recoveries ({follow['teleports']} teleports)")

    storage = read_storage(cache / "shaders" / "shareable")
    result["stored"] = {n: len(r) for n, r in storage["lists"].items()}
    result["stored_shaders"] = len(storage["shaders"])
    if not a.no_pack and pack.exists():
        pc = pack_content(pack)
        new_shaders, new_lists = created_in_play(storage, pc)
        result["new_shaders"] = len(new_shaders)
        result["new_pipelines"] = {n: len(r) for n, r in new_lists.items() if r}
        used = next((n for n in new_lists if new_lists[n]), None)
        lines = []
        for n, recs in new_lists.items():
            for r in recs:
                vs, ps = record_shaders(r)
                lines.append(f"{n} {record_key(r):016X} vs={vs:016X}{'*' if vs in new_shaders else ''} "
                             f"ps={ps:016X}{'*' if ps in new_shaders else ''}")
        (out / f"{a.tag}.new.txt").write_text("\n".join(lines) + ("\n" if lines else ""))
        result["new_with_new_shader"] = sum(1 for ln in lines if "*" in ln)
    else:
        result["new_pipelines"] = dict(result["stored"])
        result["new_shaders"] = result["stored_shaders"]
    result_path.write_text(json.dumps(result, indent=1))
    brief = {k: result.get(k) for k in ("tag", "exit", "run", "preloaded_at_start", "new_shaders",
                                       "new_pipelines", "gpu_clean", "error")}
    brief["frames"] = result.get("frames")
    print(json.dumps(brief))
    return 0 if "error" not in result else 1


# --- batch ---------------------------------------------------------------------------------------
def parse_job_line(line):
    """The arguments of a jobs file line. Not POSIX: a backslash is a character of a path, not an
    escape (the jobs files use forward slashes anyway); a token in quotes loses them."""
    return [t[1:-1] if len(t) > 1 and t[0] == t[-1] and t[0] in "\"'" else t
            for t in shlex.split(line, posix=False)]


def batch(a):
    a.out = str(checked_path(a.out, "--out", write=True))
    if a.pack:
        a.pack = str(checked_path(a.pack, "--pack"))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    lockrun = os.environ.get("COVERAGE_LOCKRUN")
    failed = 0
    for n, line in enumerate(Path(a.jobs).read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        args = parse_job_line(line)
        if "--tag" in args:
            args[args.index("--tag") + 1] += a.suffix
        if a.pack:
            args += ["--pack", a.pack]
        if a.salt_base and "--salt" not in args:
            args += ["--salt", str(a.salt_base + n)]
        tag = args[args.index("--tag") + 1] if "--tag" in args else f"job{n}"
        if (out / f"{tag}.json").exists() and "error" not in json.loads((out / f"{tag}.json").read_text()):
            print(f"[batch] {tag}: done already", flush=True)
            continue
        cmd = [sys.executable, str(Path(__file__).resolve()), "soak", *args, "--out", str(out)]
        if a.lock:
            if not lockrun:
                raise SystemExit("--lock needs COVERAGE_LOCKRUN (the path of lockrun.py)")
            cmd = [sys.executable, "-I", lockrun, "--lock", "--max-min", str(a.max_min),
                   "--log", str(out / f"{tag}.lockrun.log")] + (["--normal-priority"] if a.normal_priority else []) + ["--"] + cmd
        print(f"[batch] {time.strftime('%H:%M:%S')} {tag}", flush=True)
        r = subprocess.run(cmd, env=dict(os.environ))
        failed += r.returncode != 0
        print(f"[batch] {tag}: exit {r.returncode}", flush=True)
    print(f"[batch] done, {failed} failed")
    return 1 if failed else 0


# --- revalidate ----------------------------------------------------------------------------------
def leader_still_driving(run_folder, fps, seconds=10, metres=50):
    """Whether, in a probe's `watch` lines, an opponent was still going somewhere in the last `seconds`
    of the recording: the recording ended (RACEFRAMES) before the race did, so its route is the race's
    first part only. A leader that finished the race stands still."""
    if not run_folder or not Path(run_folder).exists():
        return False
    watched = devrun.watch(Path(run_folder))
    ticks = sorted(watched)
    if len(ticks) < 2:
        return False
    last = ticks[-1]
    earlier = [t for t in ticks if t <= last - seconds * fps]
    if not earlier:
        return False
    a, b = watched[earlier[-1]], watched[last]
    for i in range(1, min(len(a), len(b))):  # the player (0) isn't the leader
        if math.dist((a[i][0], a[i][2]), (b[i][0], b[i][2])) > metres:
            return True
    return False


def revalidate(a):
    """Probe results made before the checks existed (or with the route in the wrong place): each probe's
    route is looked up in --routes by its tag (probe-o05 -> o05.route), measured, and a probe whose route is
    shorter than --min-route-m is marked failed (its route kept aside as .rejected), so that a batch redoes
    it and skips the others."""
    routes = checked_path(a.routes, "--routes", write=True)
    folder = checked_path(a.folder, "folder", write=True)
    good = bad = 0
    for f in sorted(folder.glob("probe-*.json")):
        r = json.loads(f.read_text())
        route = routes / (r["tag"][len("probe-"):] + ".route")
        r.pop("error", None)
        if route.exists() and route_length(route) >= a.min_route_m and a.redo_truncated and \
                leader_still_driving(r.get("run_folder"), float(r.get("fixed_dt") or 60)):
            os.replace(route, str(route) + ".truncated")
            r["error"] = ("truncated: the leader was still driving when the recording ended, so the route is only "
                          "the race's first part; redone with a longer recording")
            r["route"], r["route_m"] = None, 0
            bad += 1
        elif route.exists() and route_length(route) >= a.min_route_m:
            r["route"], r["route_m"] = str(route), round(route_length(route))
            good += 1
        else:
            length = round(route_length(route)) if route.exists() else 0
            if route.exists():
                os.replace(route, str(route) + ".rejected")
            r["error"] = (f"stalled: the leader's route is only {length} m (< {a.min_route_m} m): a tip's dialog "
                          f"stopped the race (the batch of 2026-10-25 had no autocontinue); redone by the next batch")
            r["route"], r["route_m"] = None, length
            bad += 1
        f.write_text(json.dumps(r, indent=1))
    print(f"{good} probes valid, {bad} marked failed")
    return 0


# --- report --------------------------------------------------------------------------------------
def report(a):
    rows = []
    for folder in a.folders:
        for f in sorted(Path(folder).glob("*.json")):
            try:
                rows.append(json.loads(f.read_text()))
            except ValueError:
                continue
    if a.match:
        rows = [r for r in rows if re.search(a.match, r["tag"])]
    print(f"{'tag':34} {'script':20} {'api/path':10} {'preload':>7} {'new pipe':>9} {'new sh':>6} "
          f"{'>50ms':>5} {'>100':>5} {'worst':>6} {'frames':>7} {'left out':>8} {'sync ms':>7} {'nopres':>6}")
    totals = dict(runs=0, failed=0, new=0, shaders=0, over50=0, over100=0, frames=0, seconds=0.0,
                  skipped=0, sync_ms=0.0, nopresent=0, rescued=0)
    for r in rows:
        fr = r.get("frames") or {}
        new = sum((r.get("new_pipelines") or {}).values())
        totals["runs"] += 1
        totals["failed"] += bool(r.get("error"))
        totals["new"] += new
        totals["shaders"] += r.get("new_shaders", 0)
        totals["over50"] += fr.get("over50", 0)
        totals["over100"] += fr.get("over100", 0)
        totals["frames"] += fr.get("frames", 0)
        totals["seconds"] += fr.get("seconds", 0.0)
        v = r.get("visible") or {}
        totals["skipped"] += v.get("skipped", 0)
        totals["sync_ms"] += v.get("sync_ms", 0.0)
        totals["nopresent"] += v.get("nopresent", 0)
        totals["rescued"] += v.get("rescued", 0)
        print(f"{r['tag'][:34]:34} {r['script'][:20]:20} {(r['backend'][:3] + '/' + (r['path'] or '-')):10} "
              f"{r.get('preloaded_at_start', 0):7d} {new:9d} {r.get('new_shaders', 0):6d} "
              f"{fr.get('over50', 0):5d} {fr.get('over100', 0):5d} {fr.get('worst_ms', 0):6.0f} {fr.get('frames', 0):7d} "
              f"{(r.get('visible') or {}).get('skipped', 0):8d} {(r.get('visible') or {}).get('sync_ms', 0):7.0f} "
              f"{(r.get('visible') or {}).get('nopresent', 0):6d}"
              + (f"  ERROR {r['error'][:60]}" if r.get("error") else ""))
    print(f"total: {totals['runs']} runs ({totals['failed']} failed), {totals['new']} pipelines and "
          f"{totals['shaders']} shaders created in play, {totals['over50']} frames over 50 ms "
          f"({totals['over100']} over 100) in {totals['frames']} frames, {totals['seconds'] / 60:.1f} min; "
          f"{totals['skipped']} draws left out, {totals['rescued']} made on the spot, {totals['sync_ms']:.0f} ms of "
          f"pipeline work on the GPU thread, {totals['nopresent']} presents skipped")
    return 0


# --- grow ----------------------------------------------------------------------------------------
def grow(a):
    storages = []
    for folder in a.folders:
        for cache in sorted(Path(folder).glob("*/cache/shaders/shareable")):
            storages.append(cache)
    if not storages:
        raise SystemExit("no soak storages found")
    out = checked_path(a.out, "--out", write=True, allow_repo_pack=True)
    if a.previous:
        checked_path(a.previous, "--previous")
    if a.drop:
        checked_path(a.drop, "--drop")
    previous = Path(a.previous) if a.previous else (DEFAULT_PACK if DEFAULT_PACK.exists() else None)
    if previous and previous.resolve() == out.resolve():
        # The pack is being replaced: the previous one is kept aside for the comparison.
        kept = out.with_name(out.stem + ".previous")
        shutil.copyfile(previous, kept)
        previous = kept
    cmd = [sys.executable, str(HERE / "build_pack.py"), "--game", os.environ["MCLA_GAME_DATA"],
           "--out", str(out)] + (["--check"] if a.check else [])
    if previous and not a.no_previous:
        cmd += ["--previous", str(previous), "--report", str(out.with_suffix(".retired.json"))]
        if a.drop:
            cmd += ["--drop", str(a.drop)]
    cmd += [str(s) for s in storages]
    print(" ".join(cmd[:8]), f"... {len(storages)} storages", flush=True)
    return subprocess.run(cmd).returncode


def make_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("soak")
    p.add_argument("--script", required=True)
    p.add_argument("--backend", default="d3d12", choices=["d3d12", "vulkan"])
    p.add_argument("--path", choices=sorted(PATH_CVAR), help="pin the render target path "
                   "(rtv/rov on d3d12, fbo/fsi on vulkan); default: the GPU code's choice")
    p.add_argument("--out", required=True)
    p.add_argument("--tag", required=True)
    p.add_argument("--pack")
    p.add_argument("--no-pack", action="store_true")
    p.add_argument("--res", default="1")
    p.add_argument("--fixed-dt", type=int, default=60)
    p.add_argument("--fps", default=None)
    p.add_argument("--vsync", default=None)
    p.add_argument("--timeout", type=int, default=900)
    p.add_argument("--salt", type=int, default=0)
    p.add_argument("--var", action="append", default=[], metavar="NAME=VALUE",
                   help="a value for a script template's {NAME} (repeatable)")
    p.add_argument("--track", action="store_true", help="log the car's path (devrun --track): pad.csv")
    p.add_argument("--min-route-m", type=int, default=0, help="a route (made by --route-out, or driven by "
                   "--var ROUTE) shorter than this many metres is a failure: the race stalled")
    p.add_argument("--min-share", type=float, default=0.8, help="a run that drives a route for its whole "
                   "length (no --var RACEFRAMES) fails when the car covered less of it than this")
    p.add_argument("--route-out", help="write the route of the leading opponent (from a run whose script "
                   "had `watch`: arcade-race.pad WATCH=15) to this file")
    p.add_argument("--extra", nargs=argparse.REMAINDER, default=[])
    p = sub.add_parser("batch")
    p.add_argument("jobs")
    p.add_argument("--out", required=True)
    p.add_argument("--lock", action="store_true")
    p.add_argument("--normal-priority", action="store_true", default=True)
    p.add_argument("--max-min", type=float, default=40)
    p.add_argument("--pack", help="the pack every job imports (instead of the repository's)")
    p.add_argument("--salt-base", type=int, default=0, help="give job n the driver-cache salt BASE+n (a new "
                   "one each: the driver compiles what the pack lacks cold, as on a first start; needs a "
                   "dev build with tmp_shader_salt)")
    p.add_argument("--suffix", default="", help="added to every job's tag, to run a jobs file again "
                   "with another pack")
    p = sub.add_parser("revalidate")
    p.add_argument("folder")
    p.add_argument("--routes", required=True)
    p.add_argument("--min-route-m", type=int, default=2000)
    p.add_argument("--redo-truncated", action="store_true", help="also mark failed a probe whose recording "
                   "ended while the leader was still driving (the route is the race's first part only)")
    p = sub.add_parser("report")
    p.add_argument("folders", nargs="+")
    p.add_argument("--match", help="only the runs whose tag matches this regular expression")
    p = sub.add_parser("grow")
    p.add_argument("folders", nargs="+")
    p.add_argument("--out", required=True)
    p.add_argument("--check", action="store_true")
    p.add_argument("--previous", help="the pack being replaced (default: the repository's); the new "
                   "one may only lack entries of it that build_pack.py can explain")
    p.add_argument("--no-previous", action="store_true", help="a pack from nothing (after a change of "
                   "the storage versions the old one is refused anyway): nothing is compared")
    p.add_argument("--drop", help="a build_pack.py --drop file")
    return ap


def main():
    a = make_parser().parse_args()
    sys.exit({"soak": soak, "batch": batch, "report": report, "grow": grow, "revalidate": revalidate}[a.cmd](a))


if __name__ == "__main__":
    main()

"""Tests of the shader pack tools that need no game (fork-only development tool): the pack's
retirement rules in build_pack.py (what a pack may lack of the previous one) and coverage.py's
script templates and storage comparison. Prints PASS or FAIL per test.

    python test_pack.py        (needs the xxhash package; no game, no GPU)
"""
import struct
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_pack as bp  # noqa: E402
import coverage as cv  # noqa: E402

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")


def record(vs, ps, salt=0):
    """A D3D12-sized storage record (72 bytes) whose description uses these shaders."""
    desc = struct.pack("<QQQQ", vs, 0, ps, 0) + struct.pack("<Q", salt) * 4
    return struct.pack("<Q", bp.h64(desc)) + desc


def fake_pack(shaders, descriptions):
    """A read_pack-shaped dict: shaders (hashes), descriptions {list name: [records]}."""
    return {"shaders": [dict(hash=h) for h in shaders],
            "lists": {n: (b"", 64, recs) for n, recs in descriptions.items()}}


def test_retired_entries():
    a, b, c, d = 0xA1, 0xB2, 0xC3, 0xD4
    r_ab, r_cd, r_ac = record(a, b), record(c, d), record(a, c, 1)
    previous = fake_pack([a, b, c, d], {"rtv.d3d12.xpso": [r_ab, r_cd, r_ac]})
    # Nothing gone.
    s, dsc, un = bp.retired_entries(previous, previous, {}, {})
    check("retired: an identical pack retires nothing", not s and not dsc and not un)
    # c and d gone, and the check could not rebuild c: r_cd and r_ac go with their shaders;
    # d was dropped by hand.
    current = fake_pack([a, b], {"rtv.d3d12.xpso": [r_ab]})
    s, dsc, un = bp.retired_entries(previous, current, {c: "not rebuilt"}, {d: "retired by hand"})
    check("retired: shaders with a reason, their descriptions explained by them",
          set(s) == {c, d} and len(dsc) == 2 and not un, f"{len(s)} shaders, {len(dsc)} descriptions")
    # Something gone with no reason is reported.
    s, dsc, un = bp.retired_entries(previous, current, {c: "not rebuilt"}, {})
    check("retired: a shader gone for no reason is unexplained",
          un == [("shader", d)], str(un[:3]))
    # A description dropped alone, by its hash.
    current = fake_pack([a, b, c, d], {"rtv.d3d12.xpso": [r_ab, r_cd]})
    k = struct.unpack_from("<Q", r_ac)[0]
    s, dsc, un = bp.retired_entries(previous, current, {}, {k: "stale"})
    check("retired: a description dropped by its hash", not s and list(dsc) == [("rtv.d3d12.xpso", k)] and not un, "")
    s, dsc, un = bp.retired_entries(previous, current, {}, {})
    check("retired: a description gone for no reason is unexplained", un == [("rtv.d3d12.xpso", k)], str(un))
    # A pack can only grow without any reason.
    bigger = fake_pack([a, b, c, d, 0xE5], {"rtv.d3d12.xpso": [r_ab, r_cd, r_ac, record(0xE5, 0)]})
    s, dsc, un = bp.retired_entries(previous, bigger, {}, {})
    check("retired: a bigger pack retires nothing", not s and not dsc and not un)


def test_read_drops():
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "drops.txt"
        f.write_text("# a comment\n00000000000000A1 translator rejects it\n\n0000000000000B2 x\n")
        try:
            bp.read_drops(f)
            ok = False
        except SystemExit:
            ok = True  # 15 digits
        check("drops: a hash of 15 digits is refused", ok)
        f.write_text("00000000000000A1\n")
        try:
            bp.read_drops(f)
            ok = False
        except SystemExit:
            ok = True
        check("drops: a line without a reason is refused", ok)
        f.write_text("00000000000000A1 why not\n")
        check("drops: a hash and a reason", bp.read_drops(f) == {0xA1: "why not"})


def test_templates():
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / "inc.pad").write_text("wait 1s\n")
        (d / "r.route").write_text("1,2,3\n")
        t = d / "t.pad"
        t.write_text("#@default N=2   # presses\n#@default ROUTE=none\ninclude inc.pad\n"
                     "@repeat {N}: press a 4f ; wait 1s\n@if {ROUTE}: follow {ROUTE}\nshot {SHOTS}/x.png\n")
        out = d / "o.pad"
        cv.script_copy(t, d / "shots", out)
        lines = out.read_text().splitlines()
        check("template: defaults, repeat, no route, shots folder, include made absolute",
              lines[2] == f"include {d / 'inc.pad'}" and lines[3:7] == ["press a 4f", "wait 1s"] * 2
              and "follow" not in out.read_text() and lines[-1] == f"shot {d / 'shots'}/x.png", str(lines))
        cv.script_copy(t, d / "shots", out, {"N": "1", "ROUTE": "r.route"})
        lines = out.read_text().splitlines()
        check("template: variables override, a route is found beside the script and made absolute",
              lines[3:5] == ["press a 4f", "wait 1s"] and lines[5] == f"follow {d / 'r.route'}", str(lines))
        t.write_text("press a {MISSING}\n")
        try:
            cv.script_copy(t, d / "shots", out)
            ok = False
        except SystemExit:
            ok = True
        check("template: a variable with no value is refused", ok)
        # {REMAINING}: the race's frames less what the buttons took since "mark race".
        t.write_text("#@default RACEFRAMES=1000\nmark race\nwait 100f\npress lb 10f\nwait 20ms 5f\n"
                     "@repeat 2: press a 40f ; wait 60f\nwait {REMAINING}f\nmark race_end\n")
        cv.script_copy(t, d / "shots", out)
        waits = [ln for ln in out.read_text().splitlines() if ln.startswith("wait ") and ln.endswith("f")]
        check("template: {REMAINING} is the race's frames less the waits and presses since its start",
              waits[-1] == f"wait {1000 - 100 - 10 - 2 * (40 + 60)}f", waits[-1])
        cv.script_copy(t, d / "shots", out, {"RACEFRAMES": "100"})
        check("template: {REMAINING} is never negative", "wait 0f" in out.read_text())


def test_created_in_play():
    r1, r2, r3 = record(1, 2), record(3, 4), record(5, 6)
    pack = {"hash": 1, "shaders": {1, 2, 3, 4}, "lists": {"rtv.d3d12.xpso": {struct.unpack_from("<Q", r1)[0]}}}
    storage = {"shaders": {1: (False, b""), 2: (True, b""), 5: (False, b"")},
               "lists": {"rtv.d3d12.xpso": [r1, r2, r3], "fbo.vk.xpso": []}}
    new_shaders, new_lists = cv.created_in_play(storage, pack)
    check("coverage: what a run holds that the pack did not",
          set(new_shaders) == {5} and new_lists["rtv.d3d12.xpso"] == [r2, r3] and not new_lists["fbo.vk.xpso"], "")


def test_matrix():
    import shlex
    import subprocess
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        subprocess.run([sys.executable, str(HERE / "matrix.py"), str(d), "--ordered", "3", "--circuit", "2",
                        "--landmark", "1"], check=True, capture_output=True)
        parser = cv.make_parser()
        lines = [ln for f in sorted(d.glob("*.txt")) for ln in f.read_text().splitlines() if ln and ln[0] != "#"]
        tags = []
        for ln in lines:
            a = parser.parse_args(["soak", *shlex.split(ln), "--out", str(d)])
            tags.append(a.tag)
            template = HERE.parent / "scripts" / a.script
            out = d / "x.pad"
            cv.script_copy(template, d / "shots", out, dict(v.split("=", 1) for v in a.var))
        check("matrix: every generated job parses and its script template expands",
              len(lines) == 6 + 2 * (6 + 6 + 2 + 2 + 1) and len(set(tags)) == len(tags), f"{len(lines)} jobs, {len(set(tags))} tags")


def test_route_frames():
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "r.route"
        f.write_text("# route\n0,0,10\n10,0,10\n20,0,10\n")
        # 20 m at 10 m/s is 2 s: 120 frames, a quarter more, and 600
        check("route_frames: the time a route takes, a quarter more, and 10 s", cv.route_frames(f) == 150 + 600,
              str(cv.route_frames(f)))


def test_summaries():
    log = ("[x] dev pad: follow reached the end of the route: frames=10554 point=1107/1108 best=1107/1108 "
           "max_offset=5.8 m recoveries=2 teleports=1 retargets=3 popups=3 speed=28.4 m/s\n")
    f = cv.follow_result(log)
    check("follow summary: the share of the route, recoveries, teleports, popups",
          f == {"end": True, "best": 1107, "total": 1108, "share": 0.999, "recoveries": 2, "teleports": 1,
                "retargets": 3, "popups": 3}, str(f))
    stopped = log.replace("reached the end of the route", "stopped").replace("best=1107/1108", "best=500/1108")
    check("follow summary: a stopped follower is not the end, and the share is the best point's",
          cv.follow_result(stopped)["end"] is False and cv.follow_result(stopped)["share"] == 0.451
          and cv.follow_result("nothing") is None)
    v = cv.visible_cost("[x] TMPSS d3d12 frame skipped=12 rescued=1 waited=3.20 endwait=0.00 sync=5.50\n"
                        "[x] TMPSS vk frame skipped=0 rescued=0 waited=0.00 endwait=1.20 sync=2.00 nopresent=3\n")
    check("visible cost: draws left out, work on the GPU thread, presents skipped",
          v["skipped"] == 12 and v["frames_with_skips"] == 1 and v["sync_ms"] == 7.5 and v["max_sync_ms"] == 5.5
          and v["frames_sync_over_4ms"] == 1 and v["nopresent"] == 3 and v["rescued"] == 1, str(v))
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "r.route"
        f.write_text("0,0,10,5\n10,0,10,5\n20,0,10,5\n")  # a fourth column (the height) is allowed
        check("route_frames: a slower car takes longer", cv.route_frames(f, 60, 0.5) == 300 + 600
              and cv.route_length(f) == 20.0, str(cv.route_frames(f, 60, 0.5)))


def test_revalidate():
    import argparse
    import os
    import shutil
    base = cv.DEV_ROOT / f"test-revalidate-{os.getpid()}"
    try:
        (base / "routes").mkdir(parents=True)
        (base / "cov").mkdir()
        for tag, meters in (("probe-o05", 6000), ("probe-o06", 150)):
            (base / "cov" / f"{tag}.json").write_text('{"tag": "%s", "route": "C:mcvshp-devrouteso.route"}' % tag)
            points = [f"{i * 10},0,30\n" for i in range(meters // 10 + 1)]
            (base / "routes" / f"{tag[6:]}.route").write_text("# route\n" + "".join(points))
        code = cv.revalidate(argparse.Namespace(folder=str(base / "cov"), routes=str(base / "routes"), min_route_m=2000,
                                              redo_truncated=False, from_logs=False))
        import json
        good = json.loads((base / "cov" / "probe-o05.json").read_text())
        bad = json.loads((base / "cov" / "probe-o06.json").read_text())
        check("revalidate: a long probe keeps its result (route fixed), a short one is marked failed and its route set aside",
              code == 0 and "error" not in good and good["route_m"] == 6000 and good["route"].endswith("o05.route")
              and "stalled" in bad["error"] and not (base / "routes" / "o06.route").exists()
              and (base / "routes" / "o06.route.rejected").exists(), str(bad.get("error", ""))[:60])
        # A leader still driving when the recording ended (a truncated route) and one that finished.
        for name, end_speed in (("moving", 30.0), ("finished", 0.0)):
            run = base / name
            run.mkdir()
            lines = []
            for tick in range(0, 3001, 15):  # 50 s of race at 60 ticks a second
                x = min(tick, 1500) * 0.5 if end_speed == 0 else tick * 0.5
                lines.append(f"[t] [info] [core] [t1] dev pad: watch tick={tick} frame={tick} | 0.0 0.0 0.0 | {x:.3f} 0.0 5.0")
            (run / "game.log").write_text("\n".join(lines) + "\n")
        check("revalidate: a leader still driving at the end of the recording is told from one that finished",
              cv.leader_still_driving(base / "moving", 60.0) and not cv.leader_still_driving(base / "finished", 60.0)
              and not cv.leader_still_driving(base / "nothing", 60.0))
    finally:
        shutil.rmtree(base, ignore_errors=True)


def test_route_from_log():
    """A route from a run's watch lines: one reading far away that the next one undoes (every car at
    once, 600 m off and back: races 5 and 15 had it) is a glitch of the log; a jump that stays (a reset
    after a crash) starts the route over."""
    import argparse
    import os
    import shutil
    import route as route_tool
    base = cv.DEV_ROOT / f"test-route-{os.getpid()}"
    try:
        base.mkdir(parents=True)

        def run(name, xs):
            (base / name).mkdir()
            lines = [f"[t] [info] [core] [t1] dev pad: watch tick={15 * i} frame={15 * i} | 0.0 0.0 0.0 | "
                     f"{x:.3f} 0.0 5.0 | {x + 5:.3f} 0.0 5.0" for i, x in enumerate(xs)]
            (base / name / "game.log").write_text("\n".join(lines) + "\n")
            out = base / f"{name}.route"
            route_tool.from_log(argparse.Namespace(log=str(base / name), file=str(out), spacing=4.0, fps=60.0))
            return cv.route_length(out), cv.read_route(out)[0]

        steady = [i * 4.0 for i in range(100)]
        length, first = run("steady", steady)
        check("route from a log: a steady drive is all of it", abs(length - 396) < 8 and first[0] < 1, f"{length:.0f} m")
        glitch = list(steady)
        glitch[40] = 640.0  # one reading 600 m off
        length, first = run("glitch", glitch)
        check("route from a log: one reading far off that the next undoes is skipped, the route keeps its start",
              abs(length - 396) < 8 and first[0] < 1, f"{length:.0f} m from x={first[0]:.0f}")
        reset = steady[:40] + [i * 4.0 + 2000 for i in range(60)]
        length, first = run("reset", reset)
        check("route from a log: a jump that stays starts the route over",
              abs(length - 236) < 8 and first[0] > 1990, f"{length:.0f} m from x={first[0]:.0f}")
    finally:
        shutil.rmtree(base, ignore_errors=True)


def test_batch_only():
    """batch --only runs the jobs whose tag matches (without the suffix) and gives every job the salt
    of its line's number, so a subset gets the salts it would have in the whole run."""
    import argparse
    import contextlib
    import io
    import json
    import os
    import shutil
    base = cv.DEV_ROOT / f"test-batch-{os.getpid()}"
    try:
        base.mkdir(parents=True)
        jobs = base / "jobs.txt"
        jobs.write_text("# jobs\n--script s.pad --tag h-o00-v3-rtv\n--script s.pad --tag h-o03-v3-rtv\n"
                        "--script s.pad --tag h-c06-v3-rtv\n")
        for tag in ("h-o00-v3-rtv-after", "h-o03-v3-rtv-after"):
            (base / "out").mkdir(exist_ok=True)
            (base / "out" / f"{tag}.json").write_text(json.dumps({"tag": tag}))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = cv.batch(argparse.Namespace(jobs=str(jobs), out=str(base / "out"), pack=None, suffix="-after",
                                               only="^h-o0", salt_base=100, lock=False, max_min=5,
                                               normal_priority=True))
        out = buf.getvalue()
        check("batch --only: the jobs that match and have a result are skipped, the others not looked at",
              code == 0 and "h-o00-v3-rtv-after: done already" in out and "h-o03-v3-rtv-after: done already" in out
              and "c06" not in out, out.strip().replace("\n", " | ")[:100])
    finally:
        shutil.rmtree(base, ignore_errors=True)


def refused(fn, *args, **kwargs):
    try:
        fn(*args, **kwargs)
    except SystemExit as e:
        return str(e)
    return None


def test_paths():
    import argparse
    import shlex
    dev = cv.DEV_ROOT
    bs = chr(92)
    # A jobs line keeps the backslashes of a Windows path (POSIX shlex ate them: "C:mcvshp-devo12.route").
    line = f"--script x.pad --route-out C:{bs}mcv{bs}shp-dev{bs}routes{bs}o12.route --tag t"
    args = cv.parse_job_line(line)
    check("paths: a jobs line keeps the backslashes of a Windows path",
          args[args.index("--route-out") + 1] == f"C:{bs}mcv{bs}shp-dev{bs}routes{bs}o12.route", str(args))
    check("paths: what POSIX shlex made of it is refused (a drive letter and no slash)",
          refused(cv.checked_path, shlex.split(line)[3], "--route-out", write=True) is not None
          and cv.lost_backslashes("C:mcvshp-devrouteso12.route") and not cv.lost_backslashes("C:/mcv/x"))
    check("paths: a path inside the dev root is taken, for writing too",
          cv.checked_path(dev / "routes" / "o12.route", "x", write=True) == dev / "routes" / "o12.route")
    for what, value in (("relative", "routes/o12.route"), ("the owner's install", "C:/Games/Midnight Club Los Angeles/x.route"),
                        ("another drive's folder", "D:/somewhere/x.route"), ("up out of the dev root", str(dev / ".." / "x.route"))):
        check(f"paths: {what} is refused for writing", refused(cv.checked_path, value, "--route-out", write=True) is not None, value)
    check("paths: the repository is readable but not writable",
          cv.checked_path(cv.REPO / "shaderpack" / "x.shaderpack", "--pack") is not None
          and refused(cv.checked_path, cv.REPO / "shaderpack" / "x.shaderpack", "--out", write=True) is not None)
    check("paths: a pack may be written to the repository's shaderpack folder when it says so",
          cv.checked_path(cv.REPO / "shaderpack" / "x.shaderpack", "--out", write=True, allow_repo_pack=True) is not None)
    a = cv.make_parser().parse_args(["soak", "--script", "arcade-race.pad", "--tag", "t", "--out", str(dev / "o"),
                                     "--route-out", "C:/Games/Midnight Club Los Angeles/mcla-recomp/r.route"])
    check("paths: a soak refuses an output path in the owner's folder before it starts",
          refused(cv.checked_args, a) is not None)
    a = cv.make_parser().parse_args(["soak", "--script", "arcade-race.pad", "--tag", "t", "--out", str(dev / "o"),
                                     "--var", "ROUTE=C:mcvshp-devrouteso12.route"])
    check("paths: a template variable that lost its backslashes is refused", refused(cv.checked_args, a) is not None)
    a = cv.make_parser().parse_args(["soak", "--script", "arcade-race.pad", "--tag", "t", "--out", str(dev / "o"),
                                     "--var", f"ROUTE={(dev / 'routes' / 'o12.route').as_posix()}", "--var", "KIND=0"])
    check("paths: a soak with its paths inside the dev root and a route with forward slashes is taken",
          refused(cv.checked_args, a) is None)
    import devrun
    check("paths: devrun refuses a relative output, one outside the dev root, one that lost its backslashes",
          all(refused(devrun.dev_output, v, "--stdout") is not None
              for v in ("out.txt", "C:/Games/x.txt", "C:mcvshp-devx.txt"))
          and devrun.dev_output(dev / "x.txt", "--stdout") == (dev / "x.txt").resolve())


for test in (test_retired_entries, test_read_drops, test_templates, test_created_in_play, test_matrix,
             test_route_frames, test_paths, test_summaries, test_revalidate, test_route_from_log, test_batch_only):
    test()
print("ALL PASS" if all(results) else f"{results.count(False)} FAILED")
sys.exit(0 if all(results) else 1)

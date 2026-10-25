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


def test_created_in_play():
    r1, r2, r3 = record(1, 2), record(3, 4), record(5, 6)
    pack = {"hash": 1, "shaders": {1, 2, 3, 4}, "lists": {"rtv.d3d12.xpso": {struct.unpack_from("<Q", r1)[0]}}}
    storage = {"shaders": {1: (False, b""), 2: (True, b""), 5: (False, b"")},
               "lists": {"rtv.d3d12.xpso": [r1, r2, r3], "fbo.vk.xpso": []}}
    new_shaders, new_lists = cv.created_in_play(storage, pack)
    check("coverage: what a run holds that the pack did not",
          set(new_shaders) == {5} and new_lists["rtv.d3d12.xpso"] == [r2, r3] and not new_lists["fbo.vk.xpso"], "")


for test in (test_retired_entries, test_read_drops, test_templates, test_created_in_play):
    test()
print("ALL PASS" if all(results) else f"{results.count(False)} FAILED")
sys.exit(0 if all(results) else 1)

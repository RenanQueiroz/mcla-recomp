"""Tests of the shader pack's import and preparation (src/shader_prepare.cpp), through devrun.py
with private data and cache roots (fork-only development tool, Windows). Each prints PASS or FAIL.

    python test_storage.py      (MCLA_DEV_ROOT set; MCLA_GAME_DATA for a checkout without the game)
    python test_storage.py retire    only the retirement tests (R1 to R6)

- a full preparation, then a "player's" storage made of the first part of its files;
- M1: a storage file held by another program (as a running game's SDK holds it) stops a
  preparation (exit 7) and a start's import, with nothing written;
- M2: packs with a list of a name, record size, header or count this build doesn't take are
  refused (exit 3), nothing written anywhere;
- M3: the session lock: a preparation beside a game exits 7; a start beside another game neither
  waits nor imports; a start during a preparation waits for it, then imports;
- M4: a crash right after the import (mcla_shader_pack_test_crash=1) is cut back at the next
  start; two in a row disable the pack (the player's entries kept, also at a normal start); the
  retry option imports again; a crash while rebuilding (=2) counts too;
- the import at a normal start: an empty storage filled (and the stamp written), nothing missing
  (and its time), files cut mid-record completed, a file of another version replaced;
- R1 to R6, retiring what an earlier pack added and the pack in use no longer has: the ledger a
  start writes (what the import ADDED), the player's own entries never in it, a smaller pack
  retiring nothing at the start that first sees it and exactly the ledger's entries it lacks at the
  next (in order, a shader only when no description left uses it), a start that ended between the
  lists and the shader file finished by the next, a pack that lacks most of what imports added
  refused (and allowed in a preparation with --mcla_prepare_shaders_retry), no ledger or a
  damaged one retiring nothing.
"""
import ctypes
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEVRUN = str(HERE.parent / "devrun.py")
PACK = HERE.parents[2] / "shaderpack" / "545407F8.shaderpack"
PY = sys.executable
T = "545407F8"
sys.path.insert(0, str(HERE))
import build_pack as bp  # noqa: E402


def hold(argv):
    """--hold-file <path> <seconds>: the file open with no sharing, as the SDK's _wfopen_s(a+b);
    --hold-lock <lock file> <owner> <seconds>: the session lock, as another mcla.exe holds it."""
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = wintypes.HANDLE
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                              wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    if argv[0] == "--hold-file":
        h = k.CreateFileW(argv[1], 0xC0000000, 0, None, 3, 0x80, None)
        assert h != wintypes.HANDLE(-1).value, ctypes.get_last_error()
        seconds = float(argv[2])
    else:
        class OVERLAPPED(ctypes.Structure):
            _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                        ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                        ("hEvent", wintypes.HANDLE)]
        h = k.CreateFileW(argv[1], 0xC0000000, 7, None, 4, 0x80, None)
        assert h != wintypes.HANDLE(-1).value, ctypes.get_last_error()
        o = OVERLAPPED()
        o.Offset = 0x7FFFFFFF  # the byte the game locks, past the owner text
        assert k.LockFileEx(h, 3, 0, 1, 0, ctypes.byref(o)), ctypes.get_last_error()
        data = argv[2].encode()
        n = wintypes.DWORD()
        k.SetFilePointer(h, 0, None, 0)
        k.SetEndOfFile(h)
        k.WriteFile(h, data, len(data), ctypes.byref(n), None)
        seconds = float(argv[3])
    print("holding", flush=True)
    time.sleep(seconds)


def holder(*args):
    p = subprocess.Popen([PY, __file__, *args], stdout=subprocess.PIPE, text=True)
    p.stdout.readline()
    return p


class Suite:
    def __init__(self):
        self.dev = Path(os.environ["MCLA_DEV_ROOT"])
        self.work = self.dev / "storage-tests"
        self.work.mkdir(parents=True, exist_ok=True)
        self.quit = self.work / "quit.pad"
        self.quit.write_text("wait 3s\nquit\n")
        self.results = []

    def check(self, name, ok, detail=""):
        self.results.append(bool(ok))
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}", flush=True)

    def run(self, tag, cache, prepare=True, extra=(), script=None):
        out = self.work / f"{tag}.stdout"
        args = [PY, DEVRUN, "run", "--backend", "d3d12", "--res", "1", "--cache-root", str(cache),
                "--timeout", "300", "--no-gpu-guard", "--stdout", str(out)]
        if script:
            args += ["--script", str(script)]
        args += ["--extra"] + (["--mcla_prepare_shaders=true"] if prepare else []) + list(extra)
        t0 = time.monotonic()
        r = subprocess.run(args, capture_output=True, text=True)
        wall = time.monotonic() - t0
        if "real save data unchanged" not in r.stdout:
            print(r.stdout[-2000:], r.stderr[-2000:])
            raise SystemExit("devrun problem, or REAL DATA CHANGED")
        m = re.search(r"^(\d{8}-\d{6}\S*): exit (-?\d+)", r.stdout, re.M)
        lines = [l for l in out.read_text(errors="replace").splitlines()
                 if l.startswith("SHADERPREP")] if out.exists() else []
        log = (self.dev / "runs" / m.group(1) / "game.log").read_text(errors="replace")
        return int(m.group(2)), lines, log, wall

    @staticmethod
    def shareable(cache):
        return Path(cache) / "shaders" / "shareable"

    @staticmethod
    def local(cache):
        return Path(cache) / "shaders" / "local"

    def snapshot(self, cache):
        return {p.name: p.read_bytes() for p in sorted(self.shareable(cache).glob(f"{T}.*"))
                if not p.name.endswith(".tmp")}

    def fresh(self, name, seed=None):
        c = self.work / f"cache-{name}"
        if c.exists():
            shutil.rmtree(c)
        if seed:
            shutil.copytree(seed, c)
        else:
            c.mkdir(parents=True)
        return c

    def journal(self, cache):
        p = self.local(cache) / f"{T}.import"
        return p.read_text() if p.exists() else ""

    @staticmethod
    def line(lines, key):
        return next((l for l in lines if l.startswith("SHADERPREP " + key)), "")

    @staticmethod
    def prefix_kept(before, after):
        return all(n in after and after[n][:len(b)] == b for n, b in before.items())

    # --- Retiring what an earlier pack added (src/shader_prepare.cpp, RetireStale) -----------------
    @staticmethod
    def ledger_of(local_folder):
        """The ledger (<local>/545407F8.ledger) as {pack, versions, shaders, lists}, or None."""
        path = Path(local_folder) / f"{T}.ledger"
        if not path.exists():
            return None
        d = path.read_bytes()
        assert d[:8] == b"MCLALDG1" and bp.h64(d[:-8]) == struct.unpack_from("<Q", d, len(d) - 8)[0], "ledger damaged"
        fmt, title, v0, v1, v2, pack, ns, nl = struct.unpack_from("<IIIIIQII", d, 8)
        p = 8 + 4 * 5 + 8 + 8
        shaders = set(struct.unpack_from(f"<{ns}Q", d, p))
        p += 8 * ns
        lists = []
        for _ in range(nl):
            (n,) = struct.unpack_from("<I", d, p)
            lists.append(set(struct.unpack_from(f"<{n}Q", d, p + 4)))
            p += 4 + 8 * n
        assert p == len(d) - 8
        return dict(pack=pack, versions=(v0, v1, v2), shaders=shaders, lists=lists)

    @staticmethod
    def storage_content(files):
        """{list file name: [records]} and the xsh's {hash: whole record bytes} (in file order) from
        a snapshot ({file name: bytes})."""
        lists = {}
        for name, (size, _) in {"rtv.d3d12.xpso": (72, 0), "rov.d3d12.xpso": (72, 0),
                                "fbo.vk.xpso": (68, 0), "fsi.vk.xpso": (68, 0)}.items():
            d = files.get(f"{T}.{name}", b"")
            lists[name] = [d[p:p + size] for p in range(12, len(d) - size + 1, size)] if d else []
        d = files.get(f"{T}.xsh", b"")
        shaders, p = {}, 8
        while d and p + 12 <= len(d):
            hh, w = struct.unpack_from("<QI", d, p)
            n = 12 + (w & 0x7FFFFFFF) * 4
            shaders[hh] = d[p:p + n]
            p += n
        return lists, shaders

    def retire_tests(self):
        check, run, line, snapshot, journal = self.check, self.run, self.line, self.snapshot, self.journal
        shareable, local, fresh = self.shareable, self.local, self.fresh
        quit_pad = self.quit
        pk = bp.read_pack(PACK)
        names = list(bp.LISTS)  # rtv, rov, fbo, fsi: the order of the game's lists (and the ledger's)
        pack_dir = self.work / "retire-packs"
        pack_dir.mkdir(exist_ok=True)
        pack_hash = lambda f: struct.unpack_from("<Q", Path(f).read_bytes(), len(Path(f).read_bytes()) - 8)[0]
        key = lambda r: struct.unpack_from("<Q", r)[0]
        uses = lambda r: (struct.unpack_from("<Q", r, 8)[0], struct.unpack_from("<Q", r, 24)[0])

        def make_pack(name, drop_descriptions, drop_shaders):
            """The pack without these descriptions ({list name: {key}}) and shaders, and without any
            description that uses a dropped shader."""
            lists = {}
            for n, (hdr, size, recs) in pk["lists"].items():
                kept = [r for r in recs if key(r) not in drop_descriptions.get(n, set())
                        and not (set(uses(r)) & drop_shaders)]
                if kept:
                    lists[n] = (hdr, size, kept)
            shaders = [s for s in pk["shaders"] if s["hash"] not in drop_shaders]
            f = pack_dir / f"{name}.shaderpack"
            bp.write_pack(f, pk["versions"], pk["key_fp"], pk["types_fp"], shaders, lists)
            return f

        # What goes: the last 60 rtv and 30 fbo descriptions, and three vertex shaders (a, b, c)
        # with every description that uses them.
        rtv = pk["lists"]["rtv.d3d12.xpso"][2]
        vs_used = []
        for r in rtv[100:]:
            v = uses(r)[0]
            if v not in vs_used:
                vs_used.append(v)
        s_a, s_b, s_c = vs_used[:3]
        drop_s = {s_a, s_b, s_c}
        drop_d = {"rtv.d3d12.xpso": {key(r) for r in rtv[-60:]},
                  "fbo.vk.xpso": {key(r) for r in pk["lists"]["fbo.vk.xpso"][2][-30:]}}
        p1 = PACK
        p2 = make_pack("p2", drop_d, drop_s)
        h1, h2 = pack_hash(p1), pack_hash(p2)
        # Any shader's whole record, from a full import (a start on an empty storage).
        src = fresh("r-src")
        code, lines, log, _ = run("r-src", src, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p1}"])
        src_lists, src_shaders = self.storage_content(snapshot(src))
        led = self.ledger_of(local(src))
        check("R1 a start with a pack on an empty storage writes the ledger",
              code == 0 and led and led["pack"] == h1 and led["shaders"] == {s["hash"] for s in pk["shaders"]}
              and all(len(led["lists"][i]) == len(pk["lists"][n][2]) for i, n in enumerate(names))
              and "phase done" in journal(src),
              f"pack {led and format(led['pack'], '016X')}, {led and len(led['shaders'])} shaders, "
              f"{led and [len(x) for x in led['lists']]} descriptions")

        # The player's own: a description that uses a dropped shader, with that shader in the
        # storage (own a), and one whose shader only the import brings (own b).
        own_a = next(r for r in rtv[:100] + rtv if uses(r)[0] == s_a and key(r) not in drop_d["rtv.d3d12.xpso"])
        own_b = next(r for r in rtv[:100] + rtv if uses(r)[0] == s_b and key(r) not in drop_d["rtv.d3d12.xpso"])

        def seeded(name):
            c = fresh(name)
            shareable(c).mkdir(parents=True)
            (shareable(c) / f"{T}.xsh").write_bytes(b"XESH" + struct.pack(">I", pk["versions"][0]) + src_shaders[s_a])
            hdr = pk["lists"]["rtv.d3d12.xpso"][0]
            (shareable(c) / f"{T}.rtv.d3d12.xpso").write_bytes(hdr + own_a + own_b)
            return c

        def removed_expected(before_files, ledger, pack_file):
            """The records the retirement must remove, from the ledger and the pack."""
            pkx = bp.read_pack(pack_file)
            p_shaders = {s["hash"] for s in pkx["shaders"]}
            p_desc = {n: {key(r) for r in v[2]} for n, v in pkx["lists"].items()}
            lists, shaders = self.storage_content(before_files)
            gone_d = {n: {key(r) for r in lists[n] if key(r) in ledger["lists"][i]
                          and key(r) not in p_desc.get(n, set())} for i, n in enumerate(names)}
            kept_uses = set()
            for n in names:
                for r in lists[n]:
                    if key(r) not in gone_d[n]:
                        kept_uses |= {u for u in uses(r) if u}
            gone_s = {h for h in shaders if h in ledger["shaders"] and h not in p_shaders and h not in kept_uses}
            return gone_d, gone_s

        c = seeded("r2")
        code, lines, log, _ = run("r2a", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p1}"])
        led1 = self.ledger_of(local(c))
        snap1 = snapshot(c)
        check("R2 the player's own entries (a description with its shader, and one whose shader the import "
              "brings) are not in the ledger",
              led1 and key(own_a) not in led1["lists"][0] and key(own_b) not in led1["lists"][0]
              and s_a not in led1["shaders"] and s_b in led1["shaders"], "")
        # The start that first sees the smaller pack changes nothing, and the ledger becomes its.
        code, lines, log, _ = run("r2b", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p2}"])
        snap2 = snapshot(c)
        led2 = self.ledger_of(local(c))
        check("R2 the start that first sees the smaller pack retires nothing",
              self.prefix_kept(snap1, snap2) and "retired" not in log and led2 and led2["pack"] == h2
              and "phase done" in journal(c), f"ledger now pack {led2 and format(led2['pack'], '016X')}")
        # The next one retires what the pack dropped: exactly the ledger's entries that it lacks.
        gone_d, gone_s = removed_expected(snap2, led2, p2)
        code, lines, log, _ = run("r2c", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p2}"])
        snap3 = snapshot(c)
        lists2, shaders2 = self.storage_content(snap2)
        lists3, shaders3 = self.storage_content(snap3)
        ok = True
        for n in names:
            want = [r for r in lists2[n] if key(r) not in gone_d[n]]
            ok &= lists3[n][:len(want)] == want  # what stays keeps its order (the game may append new ones)
        want_s = [h for h in shaders2 if h not in gone_s]
        ok &= list(shaders3)[:len(want_s)] == want_s
        removed_d = sum(len(v) for v in gone_d.values())
        check("R2 the next start retires what the pack dropped, in order, and nothing else",
              ok and removed_d >= 90 and len(gone_s) >= 1 and f"retired {removed_d} pipeline descriptions and {len(gone_s)} shaders" in log,
              f"{removed_d} descriptions and {len(gone_s)} shaders retired")
        check("R2 the player's own description and its shader stay, and so does a dropped shader something uses",
              any(key(r) == key(own_a) for r in lists3["rtv.d3d12.xpso"]) and s_a in shaders3
              and any(key(r) == key(own_b) for r in lists3["rtv.d3d12.xpso"]) and s_b in shaders3,
              f"own a {key(own_a):016X} with {s_a:016X}, own b {key(own_b):016X} with {s_b:016X}")
        check("R2 a dropped shader nothing uses went (c)", s_c not in shaders3 and s_c in shaders2, f"{s_c:016X}")
        led3 = self.ledger_of(local(c))
        check("R2 the ledger lost what was retired",
              led3 and all(not (led3["lists"][i] & gone_d[n]) for i, n in enumerate(names)) and not (led3["shaders"] & gone_s)
              and s_b in led3["shaders"], "")
        code, lines, log, _ = run("r2d", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p2}"])
        check("R2 and then nothing more", "retired" not in log and self.prefix_kept(snap3, snapshot(c)), "")

        # A start that ended between the lists and the shader file: the lists were replaced, the
        # shader file was not. The next start finishes.
        c = seeded("r6")
        run("r6a", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p1}"])
        run("r6b", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p2}"])
        before = snapshot(c)
        ledger6 = self.ledger_of(local(c))
        gone_d, gone_s = removed_expected(before, ledger6, p2)
        lists6, shaders6 = self.storage_content(before)
        for n in ("rtv.d3d12.xpso", "fbo.vk.xpso"):
            f = shareable(c) / f"{T}.{n}"
            d = f.read_bytes()
            f.write_bytes(d[:12] + b"".join(r for r in lists6[n] if key(r) not in gone_d[n]))
        code, lines, log, _ = run("r6c", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p2}"])
        after = snapshot(c)
        lists7, shaders7 = self.storage_content(after)
        check("R6 a start that ended after the lists and before the shader file is finished by the next",
              all(s not in shaders7 for s in gone_s) and len(shaders7) <= len(shaders6) - len(gone_s) + 20
              and not any(key(r) in gone_d[n] for n in names for r in lists7[n]), f"{len(gone_s)} shaders went")

        # More than half of what imports added: refused (a start), allowed in a preparation with --retry.
        keep = {s["hash"] for s in pk["shaders"][:100]}
        p3 = make_pack("p3", {}, {s["hash"] for s in pk["shaders"]} - keep)
        c = fresh("r3")
        run("r3a", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p1}"])
        run("r3b", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p3}"])
        snap = snapshot(c)
        code, lines, log, _ = run("r3c", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p3}"])
        check("R3 a pack that lacks most of what imports added retires nothing",
              self.prefix_kept(snap, snapshot(c)) and "nothing is retired" in log, "the log says why")
        code, lines, log, _ = run("r3d", c, prepare=True, extra=[f"--mcla_shader_pack={p3}", "--mcla_prepare_shaders_retry=true"])
        small = snapshot(c)
        check("R3 --mcla_prepare_shaders_retry in a preparation retires them, and says so",
              code == 0 and line(lines, "retire ") and len(small[f"{T}.xsh"]) < len(snap[f"{T}.xsh"]) // 2,
              line(lines, "retire "))

        # No ledger (a storage an earlier build imported into), or a damaged one: nothing is retired.
        c = fresh("r4")
        shareable(c).mkdir(parents=True)
        for name, data in snap1.items():
            (shareable(c) / name).write_bytes(data)
        run("r4a", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p2}"])
        code, lines, log, _ = run("r4b", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p2}"])
        led4 = self.ledger_of(local(c))
        check("R4 no ledger: nothing retired, a ledger is made for the pack in use",
              self.prefix_kept(snap1, snapshot(c)) and "retired" not in log and led4 and led4["pack"] == h2
              and not led4["shaders"], "")
        c = fresh("r5")
        run("r5a", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p1}"])
        lf = local(c) / f"{T}.ledger"
        d = bytearray(lf.read_bytes())
        d[40] ^= 0xFF
        lf.write_bytes(bytes(d))
        snap = snapshot(c)
        run("r5b", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p2}"])
        code, lines, log, _ = run("r5c", c, prepare=False, script=quit_pad, extra=[f"--mcla_shader_pack={p2}"])
        led5 = self.ledger_of(local(c))
        check("R5 a damaged ledger is ignored and made again; nothing retired",
              self.prefix_kept(snap, snapshot(c)) and "retired" not in log and led5 and led5["pack"] == h2, "")

    def main(self):
        check, run, line, snapshot, journal = self.check, self.run, self.line, self.snapshot, self.journal
        shareable, local, fresh, prefix_kept = self.shareable, self.local, self.fresh, self.prefix_kept
        quit_pad = self.quit

        # A full storage from a preparation, then a "player's": the first part of two files.
        full = fresh("full")
        code, lines, log, _ = run("full", full)
        check("full preparation", code == 0 and line(lines, f"merge {len(bp.read_pack(PACK)['shaders'])} "),
              line(lines, "merge"))
        seed = fresh("seed")
        shareable(seed).mkdir(parents=True)
        xsh = (shareable(full) / f"{T}.xsh").read_bytes()
        p, n = 8, 0
        while p + 12 <= len(xsh) and n < 300:
            p += 12 + 4 * (struct.unpack_from("<I", xsh, p + 8)[0] & 0x7FFFFFFF)
            n += 1
        (shareable(seed) / f"{T}.xsh").write_bytes(xsh[:p])
        rtv = (shareable(full) / f"{T}.rtv.d3d12.xpso").read_bytes()
        (shareable(seed) / f"{T}.rtv.d3d12.xpso").write_bytes(rtv[:12 + 72 * 1000])
        seed_files = snapshot(seed)

        # M1: a file that can't be read stops the import before anything is written.
        c = fresh("m1", seed)
        held = holder("--hold-file", str(shareable(c) / f"{T}.xsh"), "25")
        code, lines, log, _ = run("m1", c)
        held.wait()
        check("M1 prepare with the xsh held", code == 7 and snapshot(c) == seed_files and not journal(c),
              f"exit {code}; {line(lines, 'error')}")
        held = holder("--hold-file", str(shareable(c) / f"{T}.rtv.d3d12.xpso"), "25")
        code, lines, log, _ = run("m1b", c, prepare=False, script=quit_pad)
        held.wait()
        check("M1 start with the list held", snapshot(c) == seed_files and "in use" in log,
              f"exit {code}; the game started, no import")

        # M2: lists this build doesn't take are refused, the storage untouched.
        pk = bp.read_pack(PACK)
        packs = self.work / "packs"
        packs.mkdir(exist_ok=True)
        header, size, records = pk["lists"]["rtv.d3d12.xpso"]
        variants = {
            "name-xsh": {"xsh": (header, size, records)},
            "name-path": {"/../../../a.b": (header, size, records)},
            "size-68": {"rtv.d3d12.xpso": (header, 60, [r[:68] for r in records])},
            "header": {"rtv.d3d12.xpso": (header[:4] + b"XXXX" + header[8:], size, records)},
            "twice": {"rtv.d3d12.xpso": (header, size, records)},
        }
        for name, lists in variants.items():
            f = packs / f"{name}.shaderpack"
            bp.write_pack(f, pk["versions"], pk["key_fp"], pk["types_fp"], pk["shaders"], lists)
            if name == "twice":
                # The same list twice (write_pack takes a dict): the count set to 2, the list repeated.
                d = bytearray(f.read_bytes()[:-8])
                block = d[d.find(b"rtv.d3d12.xpso"):]
                struct.pack_into("<I", d, 8 + 40, 2)
                d += block
                d += struct.pack("<Q", bp.h64(bytes(d)))
                f.write_bytes(bytes(d))
            c = fresh("m2-" + name, seed)
            code, lines, log, _ = run("m2-" + name, c, extra=[f"--mcla_shader_pack={f}"])
            stray = [x.name for x in Path(c).rglob("*")
                     if x.is_file() and x.name not in seed_files and "local" not in x.parts]
            check(f"M2 {name}", code == 3 and snapshot(c) == seed_files and not stray,
                  f"exit {code}; {line(lines, 'error')}")

        # M3: the session lock.
        c = fresh("m3", seed)
        lock = local(c) / f"{T}.lock"
        lock.parent.mkdir(parents=True, exist_ok=True)
        held = holder("--hold-lock", str(lock), "game 1234", "20")
        code, lines, log, wall = run("m3a", c)
        check("M3 prepare beside a game", code == 7 and snapshot(c) == seed_files,
              f"exit {code} in {wall:.0f} s; {line(lines, 'error')}")
        code, lines, log, wall = run("m3b", c, prepare=False, script=quit_pad)
        check("M3 start beside another game",
              prefix_kept(seed_files, snapshot(c)) and "game 1234" in log and "pipelines added" not in log and wall < 12,
              f"exit {code} in {wall:.0f} s: no wait, no import (its own new pipelines appended)")
        held.wait()
        held = holder("--hold-lock", str(lock), "prepare 999", "12")
        code, lines, log, wall = run("m3c", c, prepare=False, script=quit_pad)
        held.wait()
        after = snapshot(c)
        check("M3 start during a preparation waits, then imports",
              prefix_kept(seed_files, after) and after != seed_files and wall >= 12, f"exit {code} in {wall:.0f} s")

        # M4: a crash after the import is cut back at the next start; twice disables the pack.
        c = fresh("m4", seed)
        code, lines, log, _ = run("m4a", c, extra=["--mcla_shader_pack_test_crash=1"])
        grown = snapshot(c)
        check("M4 crash after the import",
              code == 99 and "phase merged" in journal(c) and grown != seed_files and prefix_kept(seed_files, grown),
              f"exit {code}")
        code, lines, log, _ = run("m4b", c, extra=["--mcla_shader_pack_test_crash=1"])
        check("M4 cut back, then a second crash",
              code == 99 and "rollback" in " ".join(lines) and "failures 1" in journal(c),
              f"exit {code}; {line(lines, 'rollback')}")
        code, lines, log, _ = run("m4c", c)
        check("M4 two failed starts disable the pack, the player's entries kept",
              code == 0 and line(lines, "import-disabled") and snapshot(c) == seed_files and "disabled 1" in journal(c),
              f"exit {code}; {line(lines, 'import-disabled')}")
        code, lines, log, _ = run("m4d", c, prepare=False, script=quit_pad)
        check("M4 a normal start keeps it disabled",
              prefix_kept(seed_files, snapshot(c)) and "is disabled" in log and "pipelines added" not in log,
              f"exit {code}")
        code, lines, log, _ = run("m4e", c, extra=["--mcla_prepare_shaders_retry=true"])
        after = snapshot(c)
        check("M4 retry imports again",
              code == 0 and prefix_kept(seed_files, after) and after != seed_files
              and "phase done" in journal(c) and "failures 0" in journal(c),
              f"exit {code}; {line(lines, 'merge')}")
        c = fresh("m4r", seed)
        code, lines, log, _ = run("m4f", c, extra=["--mcla_shader_pack_test_crash=2"])
        code2, lines, log, _ = run("m4g", c)
        check("M4 a crash while rebuilding counts, nothing to cut", code == 98 and code2 == 0,
              f"exits {code}, {code2}")

        # The import at a normal start.
        c = fresh("auto-empty")
        code, lines, log, wall = run("auto-empty", c, prepare=False, script=quit_pad)
        stamp = local(c) / f"{T}.stamp.json"
        st = json.loads(stamp.read_text()) if stamp.exists() else {}
        check("start: an empty storage filled, the stamp written",
              st.get("by") == "start" and st.get("pack_import") == "on" and len(snapshot(c).get(f"{T}.xsh", b"")) == len(xsh),
              f"stamp: {(st.get('storage') or {}).get('pipeline_storage')}")
        m = re.search(r"shader pack: (\d+) of (\d+) shaders rebuilt in (\d+) ms", log)
        check("start: the rebuild's time logged", bool(m), m.group(0) if m else "")
        code, lines, log, wall = run("auto-nothing", c, prepare=False, script=quit_pad)
        m = re.search(r"nothing missing from the storage \((\d+) ms\)", log)
        check("start: nothing missing", bool(m), m.group(0) if m else "")
        c = fresh("auto-cut", full)
        x = shareable(c) / f"{T}.xsh"
        x.write_bytes(x.read_bytes()[:len(xsh) // 2 + 5])
        r = shareable(c) / f"{T}.rtv.d3d12.xpso"
        r.write_bytes(r.read_bytes()[:12 + 72 * 500 + 30])
        code, lines, log, _ = run("auto-cut", c, prepare=False, script=quit_pad)
        after = snapshot(c)
        check("start: files cut mid-record completed",
              len(after[f"{T}.xsh"]) == len(xsh) and len(after[f"{T}.rtv.d3d12.xpso"]) == len(rtv), "")
        c = fresh("auto-reset", full)
        x = shareable(c) / f"{T}.xsh"
        d = bytearray(x.read_bytes())
        d[4:8] = b"\x00\x00\x00\x01"
        x.write_bytes(bytes(d))
        code, lines, log, _ = run("auto-reset", c, prepare=False, script=quit_pad)
        check("start: a file of another version replaced",
              len(snapshot(c)[f"{T}.xsh"]) == len(xsh) and "another version" in log, "")

        self.retire_tests()

        failed = self.results.count(False)
        print("ALL PASS" if not failed else f"{failed} FAILED")
        return 1 if failed else 0


def retire_only():
    suite = Suite()
    suite.retire_tests()
    failed = suite.results.count(False)
    print("ALL PASS" if not failed else f"{failed} FAILED")
    return 1 if failed else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1].startswith("--hold-"):
        hold(sys.argv[1:])
    elif sys.argv[1:] == ["retire"]:
        sys.exit(retire_only())
    else:
        sys.exit(Suite().main())

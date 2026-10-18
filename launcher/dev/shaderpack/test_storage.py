"""Tests of the shader pack's import and preparation (src/shader_prepare.cpp), through devrun.py
with private data and cache roots (fork-only development tool, Windows). Each prints PASS or FAIL.

    python test_storage.py      (MCLA_DEV_ROOT set; MCLA_GAME_DATA for a checkout without the game)

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
  (and its time), files cut mid-record completed, a file of another version replaced.
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

    def main(self):
        check, run, line, snapshot, journal = self.check, self.run, self.line, self.snapshot, self.journal
        shareable, local, fresh, prefix_kept = self.shareable, self.local, self.fresh, self.prefix_kept
        quit_pad = self.quit

        # A full storage from a preparation, then a "player's": the first part of two files.
        full = fresh("full")
        code, lines, log, _ = run("full", full)
        check("full preparation", code == 0 and line(lines, "merge 723"), line(lines, "merge"))
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

        failed = self.results.count(False)
        print("ALL PASS" if not failed else f"{failed} FAILED")
        return 1 if failed else 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1].startswith("--hold-"):
        hold(sys.argv[1:])
    else:
        sys.exit(Suite().main())

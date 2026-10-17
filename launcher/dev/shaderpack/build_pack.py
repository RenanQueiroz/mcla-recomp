"""Builds the shader pack (shaderpack/545407F8.shaderpack) that mcla.exe --mcla_prepare_shaders
reads (src/shader_prepare.cpp). Fork-only development tool.

The pack holds no game data, only:
  - per shader: its fingerprint (XXH3 of its microcode, the storage's own key), what it is
    (a vertex shader bound from an effect program, a program as it is, or a range of the
    decrypted default.xex), the fingerprint of the program it comes from (or the range's guest
    address and length), and for vertex shaders the fingerprints of the pixel shaders it was
    linked with;
  - the pipeline descriptions of the shader storage (render state and shader fingerprints), as
    the storage files hold them;
  - the storage versions, and fingerprints of two places in default.xex (the archive key and the
    vertex type table), to refuse a different executable.
validate() re-reads the pack and refuses anything else: every field is one of those, every
program fingerprint names a program of the game, every pipeline record's hash is its
description's, and no 8 bytes of the pack appear in the game's shader microcode.

Inputs: shader storages (<cache>/shaders/shareable folders: the agents' dev caches and, with the
user's agreement, copies of the user's), the extracted game (default.xex, xarchive_cache.rpf) and
the ReXGlue SDK checkout (third_party/rexglue-sdk, for the console's public executable key, which
this repository doesn't carry). Needs the xxhash package (pip install xxhash) and Windows (the
AES decryption uses Windows' bcrypt).

  python build_pack.py --game <game folder> --out <pack> <storage folder>...
      [--check]   then run the player side on it (mcla.exe through devrun.py, with private data
                  and cache roots) and keep only the shaders it rebuilds; the final pack is
                  checked again and must be accepted whole.

A vertex shader's source program is found by comparing its microcode with the game's programs
of the same size: the binding changes only vertex-fetch fields, export registers and unused
exports, so the source is the program that differs least. That comparison happens here, on the
developer's machine; the pack only names the program by its fingerprint.
"""
import argparse
import ctypes
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from collections import defaultdict
from ctypes import wintypes
from pathlib import Path

import xxhash

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
TITLE = 0x545407F8
IMAGE_BASE = 0x82000000
IMAGE_SIZE = 0x009E0000
RPF_KEY = 0x827D8AC4
DECL_TYPES = 0x827D42B0
PRESET_FOLDER = 0x6D1F986C
# The storage versions this tree's SDK writes (include/rex/graphics/shader_storage.h).
VERSION_RE = re.compile(r"k(Shader|D3D12Pipeline|VulkanPipeline)StorageVersion = (0x[0-9A-Fa-f]+)")
LISTS = {  # file name -> (description size, storage header magic_api)
    "rtv.d3d12.xpso": (64, 0x54525844),
    "rov.d3d12.xpso": (64, 0x4F525844),
    "fbo.vk.xpso": (60, 0),
    "fsi.vk.xpso": (60, 1),
}


def h64(data):
    return xxhash.xxh3_64_intdigest(data)


# --- AES through Windows' bcrypt ---------------------------------------------------------------
_bcrypt = ctypes.WinDLL("bcrypt")


def _aes(key, data, mode, iv=None):
    alg, hkey = ctypes.c_void_p(), ctypes.c_void_p()
    assert _bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(alg), "AES", None, 0) == 0
    try:
        mode_name = ctypes.create_unicode_buffer(mode)
        assert _bcrypt.BCryptSetProperty(alg, "ChainingMode", mode_name,
                                         ctypes.sizeof(mode_name), 0) == 0
        kb = ctypes.create_string_buffer(key, len(key))
        assert _bcrypt.BCryptGenerateSymmetricKey(alg, ctypes.byref(hkey), None, 0, kb,
                                                  len(key), 0) == 0
        try:
            n = len(data) // 16 * 16
            src = ctypes.create_string_buffer(bytes(data[:n]), n)
            dst = ctypes.create_string_buffer(n)
            out = wintypes.ULONG()
            ivb = ctypes.create_string_buffer(iv, 16) if iv is not None else None
            assert _bcrypt.BCryptDecrypt(hkey, src, n, None, ivb, 16 if iv is not None else 0,
                                         dst, n, ctypes.byref(out), 0) == 0
            return dst.raw[:n] + bytes(data[n:])
        finally:
            _bcrypt.BCryptDestroyKey(hkey)
    finally:
        _bcrypt.BCryptCloseAlgorithmProvider(alg, 0)


def retail_key(sdk):
    text = (sdk / "src" / "system" / "xex_module.cpp").read_text(encoding="utf-8", errors="replace")
    m = re.search(r"xe_xex2_retail_key\[16\]\s*=\s*\{([^}]*)\}", text)
    if not m:
        raise SystemExit("the console's executable key isn't in the SDK checkout")
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def decrypted_image(xex_path, sdk):
    d = xex_path.read_bytes()
    magic, flags, pe_off, _, sec_off, nhdr = struct.unpack_from(">4sIIIII", d, 0)
    assert magic == b"XEX2"
    fmt = [v for k, v in (struct.unpack_from(">II", d, 0x18 + i * 8) for i in range(nhdr)) if k == 0x3FF][0]
    size, enc, comp = struct.unpack_from(">IHH", d, fmt)
    if comp != 1:
        raise SystemExit("default.xex: unexpected compression")
    blocks = [struct.unpack_from(">II", d, fmt + 8 + j * 8) for j in range((size - 8) // 8)]
    session = _aes(retail_key(sdk), d[sec_off + 0x150:sec_off + 0x160], "ChainingModeECB")
    payload = d[pe_off:]
    if enc == 1:
        payload = _aes(session, payload, "ChainingModeCBC", b"\0" * 16)
    img = bytearray()
    p = 0
    for ds, zs in blocks:
        img += payload[p:p + ds]
        p += ds
        img += b"\0" * zs
    return bytes(img)


# --- the archive -----------------------------------------------------------------------------
def name_hash(name):
    h = 0
    for c in name.lower().encode():
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


def shader_files(rpf_path, key):
    """[(path of name hashes, data)] of the plain files under shaders/."""
    with open(rpf_path, "rb") as f:
        magic, toc_size, count, _, enc = struct.unpack("<4sIIII", f.read(20))
        f.seek(0x800)
        toc = f.read(toc_size)
        if enc:
            for _ in range(16):
                toc = _aes(key, toc, "ChainingModeECB")
        e = [struct.unpack_from("<4I", toc, i * 16) for i in range(count)]
        out = []
        stack = [(c, (e[c][0],)) for c in range(e[0][2] & 0x7FFFFFFF, (e[0][2] & 0x7FFFFFFF) + e[0][3])
                 if e[c][0] == name_hash("shaders")]
        while stack:
            i, path = stack.pop()
            if e[i][2] & 0x80000000:
                first = e[i][2] & 0x7FFFFFFF
                stack += [(c, path + (e[c][0],)) for c in range(first, first + e[i][3])]
                continue
            kind = e[i][3] >> 30
            if kind & 2:
                continue
            f.seek(e[i][2])
            data = zlib.decompress(f.read(e[i][3] & 0x3FFFFFFF), -15) if kind == 1 else f.read(e[i][1])
            out.append((path, data))
    return out


def programs(buf):
    """{fingerprint: (vertex?, ucode, blob offset)} of the shader blobs in buf (as the game finds
    them: the header's data offset and size, the program info's literal and microcode sizes)."""
    out = {}
    for m in re.finditer(rb"\x10\x2a\x11[\x00\x01]", buf):
        b = m.start()
        if b + 64 > len(buf):
            continue
        magic, doff, dsize = struct.unpack_from(">III", buf, b)
        info = struct.unpack_from(">I", buf, b + 24)[0]
        if not (0x24 <= doff < 0x10000 and 0 < dsize < 0x40000) or b + doff + dsize > len(buf) or info + 8 > doff:
            continue
        lit, n = struct.unpack_from(">II", buf, b + info)
        if lit + n != dsize or not n or n % 4:
            continue
        u = bytes(buf[b + doff + lit:b + doff + lit + n])
        out.setdefault(h64(u), (bool(magic & 1), u, b))
    return out


# --- the storages ----------------------------------------------------------------------------
def read_shaders(path):
    d = path.read_bytes()
    out = {}
    p = 8
    while p + 12 <= len(d):
        hh, w = struct.unpack_from("<QI", d, p)
        n = (w & 0x7FFFFFFF) * 4
        u = d[p + 12:p + 12 + n]
        if len(u) != n or h64(u) != hh:
            break
        out[hh] = (bool(w >> 31), u)
        p += 12 + n
    return out


def read_pipelines(path, size, header):
    d = path.read_bytes()
    if d[:12] != header:
        return []
    rec = 8 + size
    out = []
    for p in range(12, len(d) - rec + 1, rec):
        r = d[p:p + rec]
        if h64(r[8:]) != struct.unpack_from("<Q", r)[0]:
            break
        out.append(r)
    return out


def storage_versions(sdk):
    text = (sdk / "include" / "rex" / "graphics" / "shader_storage.h").read_text()
    v = dict(VERSION_RE.findall(text))
    return int(v["Shader"], 16), int(v["D3D12Pipeline"], 16), int(v["VulkanPipeline"], 16)


def headers(versions):
    def bswap(x):
        return struct.unpack("<I", struct.pack(">I", x))[0]
    out = {}
    for name, (size, api) in LISTS.items():
        ver = versions[1] if "d3d12" in name else versions[2]
        out[name] = struct.pack("<III", 0x53504558, api, bswap(ver))
    return out


# --- the pack --------------------------------------------------------------------------------
def write_pack(path, versions, key_fp, types_fp, shaders, lists):
    b = bytearray(b"MCLASPK1")
    b += struct.pack("<II3IQQII", 1, TITLE, *versions, key_fp, types_fp, len(shaders), len(lists))
    for s in shaders:
        b += struct.pack("<QBBBBIQ", s["hash"], s["kind"], s["pixel"], len(s["ps"]), 0, s["dwords"], s["source"])
        for p in s["ps"]:
            b += struct.pack("<Q", p)
    for name, (header, size, records) in lists.items():
        b += name.encode().ljust(16, b"\0") + header + struct.pack("<II", 8 + size, len(records))
        for r in records:
            b += r
    b += struct.pack("<Q", h64(bytes(b)))
    path.write_bytes(bytes(b))


def read_pack(path):
    d = path.read_bytes()
    assert d[:8] == b"MCLASPK1" and h64(d[:-8]) == struct.unpack_from("<Q", d, len(d) - 8)[0], "damaged pack"
    fmt, title, v0, v1, v2, key_fp, types_fp, ns, nl = struct.unpack_from("<II3IQQII", d, 8)
    p = 52
    shaders = []
    for _ in range(ns):
        hh, kind, pixel, nps, res, dwords, source = struct.unpack_from("<QBBBBIQ", d, p)
        p += 24
        ps = list(struct.unpack_from("<%dQ" % nps, d, p))
        p += 8 * nps
        shaders.append(dict(hash=hh, kind=kind, pixel=pixel, reserved=res, dwords=dwords, source=source, ps=ps))
    lists = {}
    for _ in range(nl):
        name = d[p:p + 16].rstrip(b"\0").decode()
        header = d[p + 16:p + 28]
        rec, n = struct.unpack_from("<II", d, p + 28)
        p += 36
        lists[name] = (header, rec - 8, [d[p + i * rec:p + (i + 1) * rec] for i in range(n)])
        p += n * rec
    assert p == len(d) - 8, "trailing bytes"
    return dict(format=fmt, title=title, versions=(v0, v1, v2), key_fp=key_fp, types_fp=types_fp,
                shaders=shaders, lists=lists, size=len(d))


def xex_range(image, s):
    off = s["source"] - IMAGE_BASE
    return image[off:off + 4 * s["dwords"]]


def validate(path, progs, image, versions):
    """Refuses a pack with anything but fingerprints, offsets/lengths and pipeline descriptions."""
    pk = read_pack(path)
    problems = []
    if pk["format"] != 1 or pk["title"] != TITLE or pk["versions"] != versions:
        problems.append("header")
    ps_fps = {k for k, v in progs.items() if not v[0]}
    vs_fps = {k for k, v in progs.items() if v[0]}
    known = set()
    for s in pk["shaders"]:
        known.add(s["hash"])
        if s["reserved"] or s["pixel"] not in (0, 1) or s["kind"] not in (0, 1, 2):
            problems.append(f"{s['hash']:016X}: unknown fields")
        elif s["kind"] == 1 and (s["source"] not in progs or s["source"] != s["hash"] or s["ps"]):
            problems.append(f"{s['hash']:016X}: not a program of the game")
        elif s["kind"] == 0 and (s["source"] not in vs_fps or s["pixel"]
                                 or any(p and p not in ps_fps for p in s["ps"])):
            problems.append(f"{s['hash']:016X}: source or pixel shaders aren't programs of the game")
        elif s["kind"] == 2 and not (IMAGE_BASE <= s["source"] and s["source"] + 4 * s["dwords"] <= IMAGE_BASE + IMAGE_SIZE
                                     and not s["ps"] and h64(xex_range(image, s)) == s["hash"]):
            problems.append(f"{s['hash']:016X}: not a shader's range of default.xex")
    hdrs = headers(versions)
    for name, (header, size, records) in pk["lists"].items():
        if name not in LISTS or header != hdrs[name] or size != LISTS[name][0]:
            problems.append(f"{name}: unknown list")
            continue
        for r in records:
            vs, ps = struct.unpack_from("<Q", r, 8)[0], struct.unpack_from("<Q", r, 24)[0]
            if h64(r[8:]) != struct.unpack_from("<Q", r)[0] or vs not in known or (ps and ps not in known):
                problems.append(f"{name}: a record isn't a description of this pack's shaders")
                break
    # No 8 bytes of game shader microcode anywhere in the pack (windows with 4 or more zero or
    # 0xFF bytes are common in any binary data and are skipped).
    windows = set()
    code = [u for vertex, u, off in progs.values()]
    code += [xex_range(image, s) for s in pk["shaders"] if s["kind"] == 2]
    for u in code:
        for i in range(0, len(u) - 7, 4):
            windows.add(u[i:i + 8])
    data = path.read_bytes()
    hits = 0
    for i in range(len(data) - 7):
        w = data[i:i + 8]
        if w.count(0) + w.count(0xFF) >= 4:
            continue
        hits += w in windows
    if hits:
        problems.append(f"{hits} windows of game microcode in the pack")
    return pk, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("storages", nargs="+", type=Path)
    ap.add_argument("--game", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=REPO / "shaderpack" / "545407F8.shaderpack")
    ap.add_argument("--sdk", type=Path, default=REPO / "third_party" / "rexglue-sdk")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--devrun-cache", type=Path, help="private shader storage folder for the check runs")
    a = ap.parse_args()

    versions = storage_versions(a.sdk)
    image = decrypted_image(a.game / "default.xex", a.sdk)
    key = image[RPF_KEY - IMAGE_BASE:RPF_KEY - IMAGE_BASE + 32]
    files = shader_files(a.game / "xarchive_cache.rpf", key)
    fxl = name_hash("fxl_final")
    progs = {}
    for path, data in files:
        if len(path) >= 2 and path[-2] == fxl:
            progs.update(programs(data))
    progs.update(programs(image[:IMAGE_SIZE]))
    print(f"{len(progs)} programs in the game ({sum(v[0] for v in progs.values())} vertex)")

    shaders, pairs = {}, defaultdict(set)
    hdrs = headers(versions)
    lists = {name: {} for name in LISTS}
    for folder in a.storages:
        for x in folder.glob("*.xsh"):
            shaders.update(read_shaders(x))
        for name, (size, api) in LISTS.items():
            for f in folder.glob(f"*.{name}"):
                for r in read_pipelines(f, size, hdrs[name]):
                    lists[name][r] = True
                    pairs[struct.unpack_from("<Q", r, 8)[0]].add(struct.unpack_from("<Q", r, 24)[0])
    print(f"{len(shaders)} shaders, pipelines: " + ", ".join(f"{n} {len(v)}" for n, v in lists.items()))

    by_size = defaultdict(list)
    for fp, (vertex, u, off) in progs.items():
        if vertex:
            by_size[len(u)].append((fp, u))
    entries = []
    for hh, (pixel, u) in sorted(shaders.items()):
        if hh in progs:
            entries.append(dict(hash=hh, kind=1, pixel=int(pixel), dwords=len(u) // 4, source=hh, ps=[]))
            continue
        off = image.find(u)
        if 0 <= off < IMAGE_SIZE and off % 4 == 0:
            entries.append(dict(hash=hh, kind=2, pixel=int(pixel), dwords=len(u) // 4, source=IMAGE_BASE + off, ps=[]))
            continue
        if pixel:
            continue
        words = struct.unpack(">%dI" % (len(u) // 4), u)
        best = None
        for fp, pu in by_size[len(u)]:
            pw = struct.unpack(">%dI" % (len(pu) // 4), pu)
            diff = sum(1 for x, y in zip(words, pw) if x != y)
            if best is None or diff < best[0]:
                best = (diff, fp)
        if best is None:
            continue
        ps = sorted(p for p in pairs.get(hh, ()) if p == 0 or p in progs)
        entries.append(dict(hash=hh, kind=0, pixel=0, dwords=len(u) // 4, source=best[1], ps=ps or [0]))
    print(f"{len(entries)} shaders have a source in the game's files; {len(shaders) - len(entries)} don't")

    def keep_lists(known):
        out = {}
        for name, recs in lists.items():
            kept = [r for r in recs if struct.unpack_from("<Q", r, 8)[0] in known and
                    (struct.unpack_from("<Q", r, 24)[0] == 0 or struct.unpack_from("<Q", r, 24)[0] in known)]
            if kept:
                out[name] = (hdrs[name], LISTS[name][0], kept)
        return out

    key_fp = h64(key)
    types_fp = h64(image[DECL_TYPES - IMAGE_BASE:DECL_TYPES - IMAGE_BASE + 64])
    a.out.parent.mkdir(parents=True, exist_ok=True)
    rounds = 2 if a.check else 0
    for round_ in range(rounds + 1):
        known = {e["hash"] for e in entries}
        write_pack(a.out, versions, key_fp, types_fp, entries, keep_lists(known))
        pk, problems = validate(a.out, progs, image, versions)
        if problems:
            a.out.unlink()
            raise SystemExit("REFUSED: " + "; ".join(problems[:10]))
        print(f"{a.out}: {pk['size']} bytes, {len(pk['shaders'])} shaders, pipelines " +
              ", ".join(f"{n} {len(v[2])}" for n, v in pk["lists"].items()) + "; validated")
        if round_ == rounds:
            break
        ok = check(a, a.out)
        failed = [e for e in entries if not ok.get(e["hash"])]
        print(f"check: {len(entries) - len(failed)} rebuilt by mcla.exe, {len(failed)} not")
        if not failed:
            break
        if round_ == rounds - 1:
            a.out.unlink()
            raise SystemExit("REFUSED: the check still fails for " + ", ".join(f"{e['hash']:016X}" for e in failed[:10]))
        entries = [e for e in entries if ok.get(e["hash"])]


def check(a, pack):
    """The player side on this pack: mcla.exe --mcla_prepare_shaders with a report and no storage
    change, through devrun.py (private save copy and cache, real data checked)."""
    report = Path(tempfile.gettempdir()) / f"shaderpack-check-{os.getpid()}.txt"
    report.unlink(missing_ok=True)
    cache = a.devrun_cache or Path(tempfile.gettempdir()) / f"shaderpack-check-cache-{os.getpid()}"
    cmd = [sys.executable, str(REPO / "launcher" / "dev" / "devrun.py"), "run", "--cache-root", str(cache),
           "--timeout", "900", "--no-gpu-guard", "--extra", "--mcla_prepare_shaders=true",
           f"--mcla_shader_pack={pack.resolve()}", f"--mcla_prepare_shaders_check={report}"]
    subprocess.run(cmd, check=False)
    if not report.exists():
        raise SystemExit("the check run wrote no report")
    ok = {}
    for line in report.read_text().splitlines():
        hh, status = line.split()
        ok[int(hh, 16)] = status == "ok"
    report.unlink()
    return ok


if __name__ == "__main__":
    main()

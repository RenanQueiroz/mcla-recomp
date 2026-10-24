r"""Fingerprints of the supported game release, for release.json's "game" field and the setup wizard's check:

  python make-game-files.py <disc image .iso | folder with default.xex> [--out scripts/ci/game-files.json]

Reads the image (or the folder) and writes the names, sizes and SHA-256 of the game's files, and the title id and
version in default.xex. Only fingerprints: nothing of the game is copied or kept. Run once, by hand, by someone
with the game; CI never has it (assemble-release.py copies the result into release.json).

The files are those scripts/extract_game.py extracts (everything on the disc except $SystemUpdate), which is
what the wizard copies: the wizard checks a disc image or a folder against this list, and the SHA-256 of each
file while it copies it.
"""
import argparse
import hashlib
import json
import os
import pathlib
import struct
import sys

SECTOR = 0x800
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITION_OFFSETS = (0x0, 0xFD90000, 0x2080000, 0x18300000, 0x89D80000)
SKIP_DIRS = ("/$SystemUpdate",)

ap = argparse.ArgumentParser()
ap.add_argument("source")
ap.add_argument("--out", default=str(pathlib.Path(__file__).with_name("game-files.json")))
a = ap.parse_args()


def sha_stream(read, size):
    h = hashlib.sha256()
    remaining = size
    while remaining:
        chunk = read(min(remaining, 1 << 24))
        if not chunk:
            sys.exit("make-game-files: the source ended inside a file")
        h.update(chunk)
        remaining -= len(chunk)
    return h.hexdigest()


def xdvdfs_files(f, size):
    for base in PARTITION_OFFSETS:
        if base + 32 * SECTOR + 28 > size:
            continue
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            root_sector, root_size = struct.unpack("<II", f.read(8))
            break
    else:
        sys.exit("make-game-files: no XDVDFS volume found")
    out = []

    def walk(sector, dir_size, path):
        f.seek(base + sector * SECTOR)
        data = f.read(dir_size)
        stack = [0]
        while stack:
            off = stack.pop() * 4
            if off + 14 > len(data):
                continue
            left, right, sec, fsize, attr, name_len = struct.unpack_from("<HHIIBB", data, off)
            if left == 0xFFFF:
                continue
            name = data[off + 14:off + 14 + name_len].decode("latin-1")
            full = f"{path}/{name}"
            if attr & 0x10:
                if fsize:
                    walk(sec, fsize, full)
            else:
                out.append((full, sec, fsize))
            if left:
                stack.append(left)
            if right:
                stack.append(right)

    walk(root_sector, root_size, "")
    return base, [e for e in sorted(out) if not e[0].startswith(SKIP_DIRS)]


def xex_info(head):
    if head[:4] != b"XEX2":
        return None
    count = struct.unpack_from(">I", head, 0x14)[0]
    for i in range(count):
        key, value = struct.unpack_from(">II", head, 0x18 + i * 8)
        if key == 0x00040006:
            _media, version, _base, title = struct.unpack_from(">IIII", head, value)
            return title, version
    return None


source = pathlib.Path(a.source)
files = []
head = b""
if source.is_dir():
    for p in sorted(source.rglob("*")):
        if p.is_file():
            rel = p.relative_to(source).as_posix()
            with open(p, "rb") as f:
                files.append({"path": rel, "size": p.stat().st_size, "sha256": sha_stream(f.read, p.stat().st_size)})
            if rel == "default.xex":
                head = p.read_bytes()[:0x4000]
else:
    size = source.stat().st_size
    with open(source, "rb") as f:
        base, entries = xdvdfs_files(f, size)
        for full, sec, fsize in entries:
            f.seek(base + sec * SECTOR)
            digest = sha_stream(f.read, fsize)
            files.append({"path": full.lstrip("/"), "size": fsize, "sha256": digest})
            if full == "/default.xex":
                f.seek(base + sec * SECTOR)
                head = f.read(0x4000)
            print(f"  {full} {fsize} {digest[:12]}", flush=True)

info = xex_info(head)
if info is None:
    sys.exit("make-game-files: default.xex is missing or not an XEX2 file")
title, version = info
version_text = f"{version >> 28}.{(version >> 24) & 0xF}.{(version >> 8) & 0xFFFF}.{version & 0xFF}"
doc = {
    "format": 1,
    "title": "Midnight Club: Los Angeles Complete Edition (USA, Europe)",
    "titleId": f"{title:08X}",
    "xexVersion": version_text,
    "files": sorted(files, key=lambda x: x["path"]),
}
pathlib.Path(a.out).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n")
print(f"make-game-files: {len(files)} files, {sum(x['size'] for x in files) / 1e9:.2f} GB, title {title:08X}, version {version_text} -> {a.out}")

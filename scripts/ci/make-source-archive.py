r"""The corresponding source of the SDK bundle, as one archive, and the facts about it:

  python make-source-archive.py --sdk <patched SDK tree> --repo <fork checkout> --out <folder>
         [--source-date-epoch N] [--no-compress]

Writes into <folder>:
  rexglue-sdk-source-<SDK commit, 8>-<patches digest, 12>.tar.xz
  sdk-source-info.json      the commits, URLs and patches of what is in it (read by make-sdk-bundle.py)

The archive holds, under one folder of the same name:
  sdk/                  the SDK as the bundle's binaries were built from it: the tree at the pinned commit
                        with every submodule (FFmpeg and the fork of it, libmspack, the PowerPC disassembler
                        that is part of the SDK, ...) at its pinned commit and every patch of the fork applied
                        (no .git folders, no build output)
  fork/                 the fork's repository at the release commit (git archive): patches/ (the changes to
                        the SDK, in the order scripts/make_pc.ps1 applies them), scripts/, src/, launcher/
  README-SOURCE.txt, SOURCE-INFO.json
The SDK tree must be one that scripts/make_pc.ps1 -Until sdk-source (or a full build) has just patched.
Deterministic: sorted entries, fixed times and owners.
"""
import argparse
import io
import json
import lzma
import pathlib
import re
import subprocess
import sys
import tarfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cilib  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--sdk", required=True)
ap.add_argument("--repo", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--source-date-epoch", type=int, default=1767225600)
ap.add_argument("--no-compress", action="store_true", help="write a plain .tar (for a quick test)")
a = ap.parse_args()
sdk, repo, out = (pathlib.Path(p).resolve() for p in (a.sdk, a.repo, a.out))
out.mkdir(parents=True, exist_ok=True)


def git(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd), *args], capture_output=True, text=True, check=True).stdout.strip()


# --- what the tree is
sdk_commit = git(sdk, "rev-parse", "HEAD")
sdk_repository = git(sdk, "remote", "get-url", "origin")
fork_commit = git(repo, "rev-parse", "HEAD")
if git(repo, "status", "--porcelain", "--untracked-files=no"):
    sys.exit("make-source-archive: the fork checkout has uncommitted changes: the archive must be of a commit")

patches = []
for line in (sdk / ".mcla-patches").read_text(encoding="ascii").splitlines():
    m = re.fullmatch(r"([0-9a-f]{64}) (\S+)", line.strip())
    if m:
        patches.append({"name": m.group(2), "sha256": m.group(1)})
if not patches:
    sys.exit("make-source-archive: the SDK has no .mcla-patches (scripts/make_pc.ps1 writes it when it patches the SDK)")
for p in patches:
    f = repo / "patches" / p["name"]
    if not f.is_file() or cilib.sha256_file(f) != p["sha256"]:
        sys.exit(f"make-source-archive: patches/{p['name']} of the fork is not the patch applied to this SDK tree")
digest = cilib.patches_digest(patches)
name = f"rexglue-sdk-source-{cilib.stem(sdk_commit, digest)}"

submodules = []
for line in git(sdk, "submodule", "status", "--recursive").splitlines():
    m = re.match(r"^([ +\-U]?)([0-9a-f]{40}) (\S+)", line)
    if not m:
        continue
    if m.group(1) == "-":
        sys.exit(f"make-source-archive: the submodule {m.group(3)} is not checked out")
    path = m.group(3)
    try:
        url = git(sdk / path, "remote", "get-url", "origin")
    except subprocess.CalledProcessError:
        url = ""
    submodules.append({"path": path, "commit": m.group(2), "url": url})

fsr = (sdk / "cmake" / "rexglue_fsr.cmake").read_text(encoding="utf-8", errors="replace")
fsr_commit = re.search(r'set\(_rexglue_fsr_commit "([0-9a-f]{40})"\)', fsr)
fsr_repository = re.search(r'set\(REXGLUE_FSR_REPOSITORY "(https://[^"]+)"', fsr)

info = {
    "format": 1,
    "archive": f"{name}.tar{'' if a.no_compress else '.xz'}",
    "sdkRepository": sdk_repository,
    "sdkCommit": sdk_commit,
    "forkCommit": fork_commit,
    "patches": cilib.sorted_patches(patches),
    "patchesDigest": digest,
    "submodules": submodules,
    "fidelityFx": {"repository": fsr_repository.group(1) if fsr_repository else None,
                   "commit": fsr_commit.group(1) if fsr_commit else None},
}

# --- the archive
readme = f"""Corresponding source of the ReXGlue SDK bundle for Midnight Club: Los Angeles (static recompilation)
=========================================================================================================

This archive holds the source of the programs in the bundle rexglue-sdk-{cilib.stem(sdk_commit, digest)}-llvm-mingw-*.zip
of the same release: rexglue.exe, rexruntime.dll, rexgpu-xenos.dll and the libraries in lib\\ (and so the
GPL and LGPL parts in them: the fork's changes, GNU binutils' PowerPC disassembler, FFmpeg's libavcodec
and libavutil, libmspack). It is kept with the release, so that it is available for as long as the
binaries are.

  sdk\\     the ReXGlue SDK tree the binaries were built from: {sdk_repository}
           commit {sdk_commit}, with every submodule at its pinned commit (SOURCE-INFO.json lists
           them, with their URLs) and the {len(patches)} patches of fork\\patches applied. It contains no .git
           folders and no build output, and none of the files whose names are not ASCII (test fixtures
           of the Vulkan loader, which no binary is built from; Windows' own tar cannot unpack them):
           SOURCE-INFO.json lists them.
  fork\\    the fork's repository at commit {fork_commit}: patches\\ (the changes to the SDK, in the
           order scripts\\make_pc.ps1 applies them), scripts\\ (how the SDK is patched and built), src\\ (the
           game's own code), launcher\\.

To build the bundle's SDK again, or a changed one: in a checkout of the fork (fork\\ here, or the
repository at that commit) run scripts\\make_pc.ps1 -Toolchain mingw -Until sdk; it clones the SDK and
its submodules from the URLs in SOURCE-INFO.json at the pinned commits, applies the patches and builds
with llvm-mingw 20250709. To use a changed SDK in place of the bundle, build it and give the install
folder to scripts\\make_pc.ps1 -SdkBundle <folder> (the libraries in lib\\ are the ones the game links, so
a changed FFmpeg or libmspack is linked into the game the same way).

Licences: sdk\\LICENSE and the licence files of each component under sdk\\thirdparty; fork\\LICENSE
(GPL-3.0-or-later) and fork\\THIRD-PARTY-NOTICES.md. The bundle's own NOTICE.txt says which binary
contains what.
"""

stamp = a.source_date_epoch
skip_names = {".git", ".mcla-patched"}


def add_bytes(tf, arcname, data, mode=0o644):
    ti = tarfile.TarInfo(arcname)
    ti.size, ti.mtime, ti.mode, ti.uid, ti.gid, ti.uname, ti.gname = len(data), stamp, mode, 0, 0, "", ""
    tf.addfile(ti, io.BytesIO(data))


entries = []  # (arcname, path)
omitted = []
for p in sdk.rglob("*"):
    rel = p.relative_to(sdk)
    if rel.parts[0] == "out" or any(part in skip_names for part in rel.parts):
        continue
    if p.is_file() and not p.is_symlink():
        if not rel.as_posix().isascii():
            # Windows' own tar cannot unpack a file with an emoji for its name (a test fixture of the
            # Vulkan loader's); a source archive that does not unpack with it is worth less than
            # the omission of a file no binary was built from. Listed in SOURCE-INFO.json.
            omitted.append(rel.as_posix())
            continue
        entries.append((f"{name}/sdk/{rel.as_posix()}", p))
entries.sort()
info["omittedNonAsciiNames"] = sorted(omitted)

target = out / info["archive"]
if target.exists():
    target.unlink()
raw = open(target, "wb")
stream = raw if a.no_compress else lzma.LZMAFile(raw, "wb", format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=6)
count = 0
with tarfile.open(fileobj=stream, mode="w|", format=tarfile.PAX_FORMAT) as tf:
    add_bytes(tf, f"{name}/README-SOURCE.txt", readme.replace("\n", "\r\n").encode("ascii"))
    add_bytes(tf, f"{name}/SOURCE-INFO.json", (json.dumps(info, indent=2) + "\n").encode("utf-8"))
    for arcname, path in entries:
        ti = tarfile.TarInfo(arcname)
        data = path.read_bytes()
        ti.size, ti.mtime, ti.mode, ti.uid, ti.gid, ti.uname, ti.gname = len(data), stamp, 0o644, 0, 0, "", ""
        tf.addfile(ti, io.BytesIO(data))
        count += 1
    # The fork at its commit, as git archive writes it.
    archived = subprocess.run(["git", "-C", str(repo), "archive", "--format=tar", "HEAD"], capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archived), mode="r:") as src:
        members = sorted((m for m in src.getmembers() if m.isfile()), key=lambda m: m.name)
        for m in members:
            data = src.extractfile(m).read()
            add_bytes(tf, f"{name}/fork/{m.name}", data, 0o755 if m.name.endswith(".sh") else 0o644)
            count += 1
if stream is not raw:
    stream.close()
raw.close()

(out / "sdk-source-info.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8", newline="\n")
print(f"make-source-archive: {target.name}: {count} files, {target.stat().st_size / 1e6:.1f} MB")
print(f"  sdk {sdk_commit[:8]} ({len(submodules)} submodules, {len(patches)} patches, digest {digest[:12]}), fork {fork_commit[:8]}")

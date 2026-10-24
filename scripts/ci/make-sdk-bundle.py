r"""Makes the release's SDK bundle zip from an installed SDK.

  python make-sdk-bundle.py --install <SDK install folder> --sdk <patched SDK source folder>
         --mingw <llvm-mingw folder> --ffx <folder with the FidelityFX DLLs> --out <folder>
         [--source-date-epoch N]

The zip holds the SDK's install tree (bin, lib, include, share, mcla-sdk-bundle.json) at its
top, which scripts\make_pc.ps1 -SdkBundle unpacks and checks, plus:

  bin\amd_fidelityfx_dx12.dll, bin\amd_fidelityfx_vk.dll   FSR 3.1's runtimes (MIT), built apart
  fidelityfx-build.json                                    what they were built from and with (commit, static C runtime, imports)
  bin\libc++.dll, bin\libunwind.dll                        llvm-mingw's C++ runtime, if the install lacks them
  licenses\                                                the licence files of the SDK and what it builds in
  NOTICE.txt, SOURCE.txt                                   what is in it, and where each part's source is

Named rexglue-sdk-<SDK commit, 8 digits>-<patches hash, 12 digits>-llvm-mingw-<version>.zip, where the
patches hash is the SHA-256 of the manifest's "name sha256" lines. The zip is deterministic (sorted,
fixed times and modes) for the same inputs. Prints the file name, size and SHA-256; with GITHUB_OUTPUT
set, also writes them as outputs (bundle_name, bundle_sha256, bundle_size, patches_hash).
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import zipfile

ap = argparse.ArgumentParser()
ap.add_argument("--install", required=True)
ap.add_argument("--sdk", required=True)
ap.add_argument("--repo", required=True, help="the fork checkout (its LICENSE and THIRD-PARTY-NOTICES.md go into the bundle)")
ap.add_argument("--mingw", required=True)
ap.add_argument("--ffx", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--stage", default="")
ap.add_argument("--source-date-epoch", type=int, default=1767225600)  # 2026-01-01
a = ap.parse_args()
install, sdk, mingw, ffx, out, repo = (pathlib.Path(p).resolve() for p in (a.install, a.sdk, a.mingw, a.ffx, a.out, a.repo))
out.mkdir(parents=True, exist_ok=True)
stage = pathlib.Path(a.stage).resolve() if a.stage else out / "stage"
if stage.exists():
    shutil.rmtree(stage)

manifest = json.loads((install / "mcla-sdk-bundle.json").read_text(encoding="utf-8-sig"))
if manifest.get("format") != 1 or manifest.get("toolchain") != "mingw":
    sys.exit("make-sdk-bundle: the install's mcla-sdk-bundle.json is not a format 1 mingw one")
sdk_commit = manifest["sdkCommit"]
lines = "".join(f"{p['name']} {p['sha256']}\n" for p in manifest["patches"])
patches_hash = hashlib.sha256(lines.encode("ascii")).hexdigest()[:12]
mingw_version = re.search(r"llvm-mingw-(\d{8})", str(mingw))
mingw_version = mingw_version.group(1) if mingw_version else sys.exit("make-sdk-bundle: --mingw is not an llvm-mingw-<date> folder")
name = f"rexglue-sdk-{sdk_commit[:8]}-{patches_hash}-llvm-mingw-{mingw_version}.zip"

# --- stage: the install tree, and what is added to it
shutil.copytree(install, stage, symlinks=False)
bin_dir = stage / "bin"
for dll in sorted(ffx.glob("amd_fidelityfx_*.dll")):
    shutil.copyfile(dll, bin_dir / dll.name)
shutil.copyfile(ffx / "fidelityfx-build.json", stage / "fidelityfx-build.json")
ffx_dlls = sorted(p.name for p in bin_dir.glob("amd_fidelityfx_*.dll"))
if ffx_dlls != ["amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll"]:
    sys.exit(f"make-sdk-bundle: the FidelityFX DLLs are {ffx_dlls}, not amd_fidelityfx_dx12.dll and amd_fidelityfx_vk.dll")
for dll in ("libc++.dll", "libunwind.dll"):
    if not (bin_dir / dll).exists():
        shutil.copyfile(mingw / "x86_64-w64-mingw32" / "bin" / dll, bin_dir / dll)

LICENSE_NAME = re.compile(r"^(licen[sc]e|licen[sc]es|copying|copying\.(lib|lgpl.*|gpl.*)|notice|unlicense)(\.(txt|md|lib))?$", re.I)
SKIP_DIRS = {"test", "tests", "testdata", "docs", "doc", "examples", "example", "third_party", ".git", "kokoro", "build"}
licenses = stage / "licenses"
licenses.mkdir()
components = []  # (directory name, licence found, files)

def detect(text):
    t = text[:20000]
    found = []
    if re.search(r"GNU LESSER GENERAL PUBLIC LICENSE|GNU LIBRARY GENERAL PUBLIC LICENSE", t): found.append("LGPL")
    elif re.search(r"GNU GENERAL PUBLIC LICENSE", t): found.append("GPL")
    if re.search(r"Apache License", t): found.append("Apache-2.0")
    if re.search(r"Boost Software License", t): found.append("BSL-1.0")
    if re.search(r"free and unencumbered software", t): found.append("Unlicense")
    if re.search(r"zlib", t, re.I) and re.search(r"altered source versions", t, re.I): found.append("zlib")
    if re.search(r"Permission is hereby granted, free of charge", t): found.append("MIT")
    if re.search(r"Redistribution and use in source and binary forms", t): found.append("BSD")
    return "/".join(found)

# Where a component's licence files name several licences, the one that applies to what the SDK builds.
KNOWN = {
    "FFmpeg": "LGPL-2.1-or-later (the SDK's configuration: CONFIG_GPL 0, CONFIG_NONFREE 0; the GPL and LGPLv3 texts in the folder are FFmpeg's other options)",
    "xxHash": "BSD-2-Clause (the library; the GPL-2.0 one in cli/ is the command-line tool, not built)",
    "libmspack": "LGPL-2.1",
    "sdl3": "zlib",
    "glslang": "BSD-3-Clause, with Apache-2.0 and MIT files (see its LICENSE.txt)",
    "catch2": "BSL-1.0 (tests only, not built into the SDK)",
}

def collect(directory, label, depth=2):
    files = []
    for path in sorted(directory.rglob("*")):
        rel = path.relative_to(directory)
        if len(rel.parts) > depth or any(part.lower() in SKIP_DIRS for part in rel.parts[:-1]):
            continue
        if path.is_file() and (LICENSE_NAME.match(path.name) or (len(rel.parts) == 2 and rel.parts[0].upper() == "LICENSES")):
            files.append(path)
    if not files:
        return
    kinds = []
    for f in files:
        target = licenses / label / f.relative_to(directory)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(f, target)
        kinds.append(detect(f.read_text(encoding="utf-8", errors="replace")))
    components.append((label, [KNOWN[label]] if label in KNOWN else sorted({k for k in kinds if k}), [str(f.relative_to(directory)).replace("\\", "/") for f in files]))

shutil.copyfile(sdk / "LICENSE", licenses / "rexglue-sdk-LICENSE.txt")
shutil.copyfile(repo / "LICENSE", licenses / "mcla-fork-LICENSE.txt")
shutil.copyfile(repo / "THIRD-PARTY-NOTICES.md", licenses / "mcla-fork-THIRD-PARTY-NOTICES.md")
for d in sorted((sdk / "thirdparty").iterdir()):
    if d.is_dir():
        collect(d, d.name)
shutil.copyfile(mingw / "LICENSE.TXT", licenses / "llvm-mingw-LICENSE.TXT")
shutil.copyfile(ffx / "LICENSE-FidelityFX.txt", licenses / "FidelityFX-LICENSE.txt")

def git(*args, cwd=sdk):
    return subprocess.run(["git", "-C", str(cwd), *args], capture_output=True, text=True, check=True).stdout.strip()

# --- SOURCE.txt: where each part's source is
subs = []
status = git("submodule", "status", "--recursive")
for line in status.splitlines():
    m = re.match(r"^[ +\-U]?([0-9a-f]{40}) (\S+)", line)
    if m:
        path = m.group(2)
        url = subprocess.run(["git", "-C", str(sdk), "config", "--file", ".gitmodules", "--get", f"submodule.{path}.url"],
                             capture_output=True, text=True).stdout.strip()
        subs.append((path, m.group(1), url))
repo_url = git("remote", "get-url", "origin")
src = [
    "Where the source of this bundle's parts is",
    "========================================",
    "",
    "This bundle is built from public sources, by the scripts in the fork it was released from",
    "(its release.json names the commit). Everything here can be built again from them.",
    "",
    "ReXGlue SDK",
    f"    {repo_url}",
    f"    commit {sdk_commit}, with the patches in patches/ of the fork, applied in the order scripts/make_pc.ps1 lists",
    f"    ({len(manifest['patches'])} patches; names and SHA-256 in mcla-sdk-bundle.json)",
    "",
    "Components the SDK builds from its third-party folder (git submodules at these commits):",
]
for path, commit, url in subs:
    src.append(f"    {path:34s} {commit}  {url}")
src += [
    "",
    "FFmpeg (libavcodec, libavutil: the XMA decoders) is compiled under the LGPL version 2.1 or later",
    "(its config header says CONFIG_GPL 0, CONFIG_NONFREE 0), and libmspack under the LGPL 2.1; both",
    "are built from the sources above into the SDK's libraries (lib\\ and the DLLs in bin\\). The scripts of",
    "the fork build the SDK, and the game against it, from these sources on any PC, so they can be",
    "replaced by building the SDK again with other sources.",
    "",
    "FidelityFX (amd_fidelityfx_dx12.dll, amd_fidelityfx_vk.dll): AMD FidelityFX SDK, MIT",
]
fsr = (sdk / "cmake" / "rexglue_fsr.cmake").read_text(encoding="utf-8", errors="replace")
fc = re.search(r'set\(_rexglue_fsr_commit "([0-9a-f]{40})"\)', fsr)
fr = re.search(r'set\(REXGLUE_FSR_REPOSITORY "(https://[^"]+)"', fsr)
src += [
    f"    {fr.group(1) if fr else '(see cmake/rexglue_fsr.cmake in the SDK)'}",
    f"    commit {fc.group(1) if fc else '(see cmake/rexglue_fsr.cmake in the SDK)'}, built by scripts/ci/build-fidelityfx.ps1 of the fork",
    "    (Clang 20.1.8 for Windows, the C runtime linked in statically)",
    "",
    f"libc++.dll, libunwind.dll: llvm-mingw {mingw_version} (LLVM's libc++ and libunwind, Apache-2.0 with the LLVM exception)",
    "    https://github.com/mstorsjo/llvm-mingw",
    "",
]
(stage / "SOURCE.txt").write_text("\r\n".join(src), encoding="utf-8", newline="")

# --- NOTICE.txt
note = [
    "ReXGlue SDK bundle for Midnight Club: Los Angeles (static recompilation)",
    "=======================================================================",
    "",
    f"SDK commit {sdk_commit}, {len(manifest['patches'])} patches, built with llvm-mingw {mingw_version}",
    "(mcla-sdk-bundle.json). This is a build of free software and contains no game data.",
    "SOURCE.txt says where the source of each part is. The licence files are in licenses\\.",
    "",
    "Licences, as found in each component's own licence files (detected from their text, so look",
    "at the files for the exact terms):",
    "",
    f"    {'the fork (patches to the SDK)':34s} GPL-3.0-or-later; licenses\\mcla-fork-LICENSE.txt",
    f"    {'':34s} notices for code in them ported from MIT projects: licenses\\mcla-fork-THIRD-PARTY-NOTICES.md",
    f"    {'ReXGlue SDK':34s} BSD-3-Clause (Tom Clay; portions derived from Xenia, Ben Vanik and the Xenia project contributors); licenses\\rexglue-sdk-LICENSE.txt",
]
for label, kinds, files in components:
    note.append(f"    {label:34s} {' / '.join(kinds) if kinds else '(see the files)'}; licenses\\{label}\\")
have_files = {label for label, _, _ in components}
no_file = {
    "disasm": "GPL-2.0-or-later: the PowerPC disassembler of GNU binutils; the notice is at the top of its source files",
    "stb": "public domain or MIT, at your choice: the end of stb_image.h",
    "renderdoc": "MIT: the top of renderdoc_app.h (a header)",
    "dxc": "NCSA, as its README.md says (headers only)",
    "dxbc": "AMD's, as the top of DXBCChecksum.cpp says",
    "pe": "Microsoft's NT image header, through Xenia: the top of pe_image.h",
    "tiny-aes-c": "Unlicense, per the tiny-AES-c project (no licence file in the SDK's copy)",
    "disruptorplus": "no licence file in the SDK's copy (headers only): the project is Disruptor-Plus",
    "ffmpeg-overlay": "part of the FFmpeg build (see FFmpeg above)",
}
note += [
    f"    {'FidelityFX (the two DLLs)':34s} MIT; licenses\\FidelityFX-LICENSE.txt",
    f"    {'libc++, libunwind (llvm-mingw)':34s} Apache-2.0 with LLVM exception; licenses\\llvm-mingw-LICENSE.TXT",
    "",
    "Components of the SDK's third-party folder that have no licence file of their own there (what each",
    "says in its source files, as far as the SDK's copy shows):",
    "",
]
for d in sorted(p.name for p in (sdk / "thirdparty").iterdir() if p.is_dir()):
    if d not in have_files:
        note.append(f"    {d:34s} {no_file.get(d, '(look at its source files; SOURCE.txt has the commit)')}")
note += [
    "",
    "FFmpeg and libmspack are LGPL (SOURCE.txt says where their source is and how the bundle is built from",
    "it). The PowerPC disassembler (disasm) is GNU binutils code under the GPL version 2 or later. The",
    "SDK's own code is BSD-3-Clause.",
    "",
]
(stage / "NOTICE.txt").write_text("\r\n".join(note), encoding="utf-8", newline="")

# --- the zip, deterministic
zip_path = out / name
if zip_path.exists():
    zip_path.unlink()
epoch = max(a.source_date_epoch, 315532800)
import time
t = time.gmtime(epoch)[:6]
files = sorted(p for p in stage.rglob("*") if p.is_file())
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for p in files:
        rel = p.relative_to(stage).as_posix()
        info = zipfile.ZipInfo(rel, date_time=t)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = (0o100755 if p.suffix.lower() in (".exe", ".dll") else 0o100644) << 16
        info.create_system = 3
        with open(p, "rb") as f:
            z.writestr(info, f.read(), compresslevel=9)
sha = hashlib.sha256(zip_path.read_bytes()).hexdigest()
size = zip_path.stat().st_size
print(f"make-sdk-bundle: {name}")
print(f"  {len(files)} files, {sum(p.stat().st_size for p in files) / 1e6:.1f} MB unpacked, {size / 1e6:.1f} MB zipped")
print(f"  sha256 {sha}")
print(f"  patches hash {patches_hash}, sdk commit {sdk_commit}")
for label, kinds, fl in components:
    print(f"  licence: {label:24s} {'/'.join(kinds) if kinds else '??'}")
if os.environ.get("GITHUB_OUTPUT"):
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as f:
        f.write(f"bundle_name={name}\nbundle_sha256={sha}\nbundle_size={size}\npatches_hash={patches_hash}\nsdk_commit={sdk_commit}\n")
shutil.rmtree(stage)

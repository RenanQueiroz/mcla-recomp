r"""Makes the release's SDK bundle zip from an installed SDK.

  python make-sdk-bundle.py --install <SDK install folder> --sdk <patched SDK source folder>
         --source-info <sdk-source-info.json> --repo <the fork's checkout> --mingw <llvm-mingw folder>
         --ffx <folder with the FidelityFX DLLs> --out <folder> [--stage <folder>] [--source-date-epoch N]

The zip holds the SDK's install tree (bin, lib, include, share, mcla-sdk-bundle.json) at its
top, which scripts\make_pc.ps1 -SdkBundle unpacks and checks, plus:

  bin\amd_fidelityfx_dx12.dll, bin\amd_fidelityfx_vk.dll   FSR 3.1's runtimes (MIT), built apart
  fidelityfx-build.json                                    what they were built from and with (commit, static C runtime, imports)
  bin\libc++.dll, bin\libunwind.dll                        llvm-mingw's C++ runtime, if the install lacks them
  fork-patches\                                            the fork's patches to the SDK (GPL-3.0-or-later), the ones in the manifest
  licenses\                                                the licence files of the SDK, the fork and what is built in
  NOTICE.txt, SOURCE.txt                                   what is in it (which binary contains what), and where the source is

The source of all of it is the release's source archive (make-source-archive.py), whose
sdk-source-info.json is given here: the SDK tree and its patches must be the ones the install was built
from (the manifest of the install must list the same patches). Named
rexglue-sdk-<SDK commit, 8>-<patches digest, 12>-llvm-mingw-<version>.zip. The zip is deterministic
(sorted, fixed times and modes) for the same inputs. Prints the file name, size and SHA-256; with
GITHUB_OUTPUT set, also writes them as outputs (bundle_name, bundle_sha256, bundle_size, patches_digest).
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cilib  # noqa: E402
zipdir = cilib.import_sibling("zipdir")

ap = argparse.ArgumentParser()
ap.add_argument("--install", required=True)
ap.add_argument("--sdk", required=True)
ap.add_argument("--source-info", required=True)
ap.add_argument("--repo", required=True, help="the fork checkout (its LICENSE, THIRD-PARTY-NOTICES.md and patches/ go into the bundle)")
ap.add_argument("--mingw", required=True)
ap.add_argument("--ffx", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--stage", default="")
# The date of every file in the zip. Leave it: a zip holds local times, so a date of "now" would unpack
# into the future on a PC west of UTC, and Ninja would find the SDK's files forever newer than its outputs.
ap.add_argument("--source-date-epoch", type=int, default=1767225600)  # 2026-01-01
a = ap.parse_args()
install, sdk, mingw, ffx, out, repo = (pathlib.Path(p).resolve() for p in (a.install, a.sdk, a.mingw, a.ffx, a.out, a.repo))
out.mkdir(parents=True, exist_ok=True)
stage = pathlib.Path(a.stage).resolve() if a.stage else out / "stage"
if stage.exists():
    shutil.rmtree(stage)
info = json.loads(pathlib.Path(a.source_info).read_text(encoding="utf-8"))

manifest = json.loads((install / "mcla-sdk-bundle.json").read_text(encoding="utf-8-sig"))
if manifest.get("format") != 1 or manifest.get("toolchain") != "mingw":
    sys.exit("make-sdk-bundle: the install's mcla-sdk-bundle.json is not a format 1 mingw one")
sdk_commit = manifest["sdkCommit"]
digest = cilib.patches_digest(manifest["patches"])
if sdk_commit != info["sdkCommit"] or digest != info["patchesDigest"]:
    sys.exit("make-sdk-bundle: the installed SDK was not built from the source tree of the source archive "
             f"(commit {sdk_commit[:8]} against {info['sdkCommit'][:8]}, patches {digest[:12]} against {info['patchesDigest'][:12]})")
mingw_version = re.search(r"llvm-mingw-(\d{8})", str(mingw))
mingw_version = mingw_version.group(1) if mingw_version else sys.exit("make-sdk-bundle: --mingw is not an llvm-mingw-<date> folder")
name = f"rexglue-sdk-{cilib.stem(sdk_commit, digest)}-llvm-mingw-{mingw_version}.zip"

# --- stage: the install tree, and what is added to it
shutil.copytree(install, stage, symlinks=False)
# pkg-config files with the build machine's prefix (nothing uses them)
shutil.rmtree(stage / "lib" / "pkgconfig", ignore_errors=True)
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

# the patches, checked against the manifest
patch_dir = stage / "fork-patches"
patch_dir.mkdir()
for p in manifest["patches"]:
    src = repo / "patches" / p["name"]
    if not src.is_file() or cilib.sha256_file(src) != p["sha256"]:
        sys.exit(f"make-sdk-bundle: patches/{p['name']} of the fork is not the patch the SDK was built with")
    shutil.copyfile(src, patch_dir / p["name"])
shutil.copyfile(repo / "patches" / "README.md", patch_dir / "README.md")

LICENSE_NAME = re.compile(r"^(licen[sc]e|licen[sc]es|copying|copying\.(lib|lgpl.*|gpl.*)|notice|unlicense)(\.(txt|md|lib))?$", re.I)
SKIP_DIRS = {"test", "tests", "testdata", "docs", "doc", "examples", "example", "third_party", ".git", "kokoro", "build"}
licenses = stage / "licenses"
licenses.mkdir()
components = []  # (directory name, licence found, files)

# Where a component's licence files name several licences, the one that applies to what the SDK builds.
KNOWN = {
    "FFmpeg": "LGPL-2.1-or-later (the SDK's configuration: CONFIG_GPL 0, CONFIG_NONFREE 0; the GPL and LGPLv3 texts in the folder are FFmpeg's other options)",
    "xxHash": "BSD-2-Clause (the library; the GPL-2.0 one in cli/ is the command-line tool, not built)",
    "libmspack": "LGPL-2.1",
    "sdl3": "zlib",
    "glslang": "BSD-3-Clause, with Apache-2.0 and MIT files (see its LICENSE.txt)",
    "catch2": "BSL-1.0 (tests only, not built into the SDK)",
}


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
shutil.copyfile(repo / "LICENSE", licenses / "GPL-3.0.txt")
shutil.copyfile(repo / "THIRD-PARTY-NOTICES.md", licenses / "mcla-fork-THIRD-PARTY-NOTICES.md")
# The GPL and LGPL texts that go with the GPL and LGPL parts, named by what they are.
shutil.copyfile(sdk / "thirdparty" / "FFmpeg" / "COPYING.GPLv2", licenses / "GPL-2.0.txt")
shutil.copyfile(sdk / "thirdparty" / "FFmpeg" / "COPYING.LGPLv2.1", licenses / "LGPL-2.1.txt")
for d in sorted((sdk / "thirdparty").iterdir()):
    if d.is_dir():
        collect(d, d.name)
shutil.copyfile(mingw / "LICENSE.TXT", licenses / "llvm-mingw-LICENSE.TXT")
mingw_notices = mingw / "x86_64-w64-mingw32" / "share" / "mingw32"
for f in ("COPYING.MinGW-w64-runtime.txt", "COPYING.winpthreads.txt"):
    shutil.copyfile(mingw_notices / f, licenses / f"mingw-w64-{f}")
shutil.copyfile(ffx / "LICENSE-FidelityFX.txt", licenses / "FidelityFX-LICENSE.txt")

# --- which binary contains what, found in the binaries themselves
MARKERS = [
    ("GNU binutils' PowerPC disassembler (opcodes), GPL-2.0-or-later", (b"print_insn_powerpc", b"powerpc_opcodes")),
    ("FFmpeg libavcodec / libavutil, LGPL-2.1-or-later", (b"avcodec_license", b"LGPL version 2.1 or later")),
    ("libmspack, LGPL-2.1", (b"mspack_",)),
]
contents = {}
for f in sorted(bin_dir.glob("*.*")):
    # The SDK's own programs; not the FidelityFX DLLs (AMD's) or llvm-mingw's libc++ and libunwind.
    if f.suffix.lower() in (".exe", ".dll") and not f.name.startswith(("amd_fidelityfx", "libc++", "libunwind")):
        data = f.read_bytes()
        contents[f.name] = [label for label, needles in MARKERS if any(n in data for n in needles)]

# --- SOURCE.txt: where each part's source is
archive = info["archive"]
src = [
    "Where the source of this bundle's parts is",
    "========================================",
    "",
    "The corresponding source of everything in this bundle's binaries is the release's source archive,",
    f"  {archive}",
    "which is kept with this bundle's release for as long as the bundle is (a written offer is not needed:",
    "download it from the same page). It holds the SDK tree this bundle was built from, with every",
    "submodule at the commits below and every patch applied, and the fork's repository at the commit that",
    "was released. Its README-SOURCE.txt says how to build the SDK again. The patches themselves are",
    "also in this zip, in fork-patches\\.",
    "",
    f"Fork:  commit {info['forkCommit']}  (https://github.com/{os.environ.get('GITHUB_REPOSITORY', 'RenanQueiroz/mcla-recomp')})",
    "",
    "ReXGlue SDK",
    f"    {info['sdkRepository']}",
    f"    commit {sdk_commit}, with the {len(manifest['patches'])} patches of fork-patches\\, applied in the order scripts/make_pc.ps1 lists",
    f"    (names and SHA-256 in mcla-sdk-bundle.json; their digest is {digest})",
    "",
    "Components the SDK builds from its third-party folder (git submodules at these commits):",
]
for sm in info["submodules"]:
    src.append(f"    {sm['path']:36s} {sm['commit']}  {sm['url']}")
src += [
    "",
    "FFmpeg (libavcodec, libavutil: the XMA decoders) is compiled under the LGPL version 2.1 or later",
    "(its config header says CONFIG_GPL 0, CONFIG_NONFREE 0; the fork of FFmpeg named above is the one",
    "built), and libmspack under the LGPL 2.1; both are built from the sources above into the SDK's",
    "libraries (lib\\ and rexruntime.dll in bin\\). To use a changed version of either, build the SDK again",
    "from the source archive with the change and give the resulting install folder to",
    "scripts\\make_pc.ps1 -SdkBundle <folder>: the game is linked against the libraries of that folder",
    "(lib\\*.a and the import libraries), so a changed library is linked into the game on the player's PC.",
    "",
    "FidelityFX (amd_fidelityfx_dx12.dll, amd_fidelityfx_vk.dll): AMD FidelityFX SDK, MIT",
    f"    {info['fidelityFx']['repository']}",
    f"    commit {info['fidelityFx']['commit']}, built by scripts/ci/build-fidelityfx.ps1 of the fork (in the source archive)",
    "    (Clang 20.1.8 for Windows, the C runtime linked in statically: the DLLs contain Microsoft's C runtime",
    "    library code, which its licence lets be redistributed this way)",
    "",
    f"libc++.dll, libunwind.dll: llvm-mingw {mingw_version} (LLVM's libc++ and libunwind, Apache-2.0 with the LLVM exception;",
    "mingw-w64 runtime objects and winpthreads, whose notices are in licenses\\mingw-w64-*)",
    "    https://github.com/mstorsjo/llvm-mingw",
    "",
]
(stage / "SOURCE.txt").write_text("\r\n".join(src), encoding="utf-8", newline="")

# --- NOTICE.txt
note = [
    "ReXGlue SDK bundle for Midnight Club: Los Angeles (static recompilation)",
    "=======================================================================",
    "",
    f"SDK commit {sdk_commit}, {len(manifest['patches'])} patches, built with llvm-mingw {mingw_version}, from fork commit {info['forkCommit']}",
    "(mcla-sdk-bundle.json). This is a build of free software and contains no game data.",
    "SOURCE.txt says where the source is (the release's source archive holds all of it). The licence files are in licenses\\.",
    "",
    "What is GPL and LGPL, as built",
    "------------------------------",
    "",
    "The programs in bin\\ are built from the ReXGlue SDK (BSD-3-Clause) with the fork's patches",
    "(GPL-3.0-or-later; licenses\\GPL-3.0.txt) and the libraries below linked in. Found in the files themselves:",
    "",
]
for fname, found in contents.items():
    note.append(f"    {fname}: " + "; ".join(["the SDK and the fork's changes (GPL-3.0-or-later)"] + found))
note += [
    "    libc++.dll, libunwind.dll: LLVM's, unmodified, from llvm-mingw (Apache-2.0 with the LLVM exception)",
    "    amd_fidelityfx_dx12.dll, amd_fidelityfx_vk.dll: AMD's FidelityFX (MIT), with Microsoft's C runtime linked in",
    "",
    "So: rexglue.exe contains GPL-2.0-or-later code (the disassembler) and the fork's GPL-3.0-or-later changes,",
    "and is distributed under the GPL version 3 or later (texts: licenses\\GPL-2.0.txt, licenses\\GPL-3.0.txt).",
    "rexruntime.dll contains the LGPL libraries FFmpeg (version 2.1 or later) and libmspack (version 2.1) with",
    "the fork's GPL-3.0-or-later changes (licenses\\LGPL-2.1.txt). The source is in the source archive; the",
    "static libraries in lib\\ and the import libraries let the game be linked again with a changed library",
    "(SOURCE.txt). The two FidelityFX DLLs are MIT (AMD) with Microsoft's C runtime linked in.",
    "",
    "Licences of what the SDK builds from, as found in each component's own licence files (detected from",
    "their text, so look at the files for the exact terms):",
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
    f"    {'mingw-w64 runtime, winpthreads':34s} licenses\\mingw-w64-COPYING.MinGW-w64-runtime.txt, licenses\\mingw-w64-COPYING.winpthreads.txt",
    "",
    "Components of the SDK's third-party folder that have no licence file of their own there (what each",
    "says in its source files, as far as the SDK's copy shows):",
    "",
]
for d in sorted(p.name for p in (sdk / "thirdparty").iterdir() if p.is_dir()):
    if d not in have_files:
        note.append(f"    {d:34s} {no_file.get(d, '(look at its source files; SOURCE.txt has the commit)')}")
note.append("")
(stage / "NOTICE.txt").write_text("\r\n".join(note), encoding="utf-8", newline="")

# --- the zip, deterministic
zip_path = out / name
count, unpacked = zipdir.write_zip(stage, zip_path, a.source_date_epoch)
sha = cilib.sha256_file(zip_path)
size = zip_path.stat().st_size
print(f"make-sdk-bundle: {name}")
print(f"  {count} files, {unpacked / 1e6:.1f} MB unpacked, {size / 1e6:.1f} MB zipped")
print(f"  sha256 {sha}")
print(f"  patches digest {digest}, sdk commit {sdk_commit}, fork commit {info['forkCommit']}")
for fname, found in contents.items():
    print(f"  contains: {fname:20s} {'; '.join(found) if found else '-'}")
for label, kinds, fl in components:
    print(f"  licence: {label:24s} {'/'.join(kinds) if kinds else '??'}")
if os.environ.get("GITHUB_OUTPUT"):
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as f:
        f.write(f"bundle_name={name}\nbundle_sha256={sha}\nbundle_size={size}\npatches_digest={digest}\nsdk_commit={sdk_commit}\n")
shutil.rmtree(stage)

r"""Writes the files of a draft release that are not built, from the built ones:

  python assemble-release.py --tag TAG --commit SHA --repository OWNER/NAME --assets <folder>
         --launcher-zip <zip> --installer <MCLA-Setup.exe> --sdk-bundle-zip <zip> --sdk-source-archive <tar.xz>
         --launcher-notices <folder> --expect-bundle-sha256 <hash> --out <folder> [--run-url URL]

  release.json     the one file the wizard reads: what the release holds, with the SHA-256 and size
                   of each asset, the SDK bundle's commit/patches/toolchain, the tools to download
                   (URL and SHA-256 each), the shader pack, and "game": the fingerprints of the supported game (names, sizes,
                   SHA-256 of its files, title id and version; never content). "installer" is MCLA-Setup.exe: its name,
                   SHA-256 and size, and what its payload header says (version, tag, commit, the launcher's files).
                   "commit" is the commit the release was built from: the tag, once it exists, has to
                   point at it (the wizard checks that).
  NOTICES.txt      the licences and notices of what the release's assets hold
  SHA256SUMS       the SHA-256 of every asset (MCLA-Setup.exe, the three archives, release.json, NOTICES.txt)
  release-notes.md the text of the draft release

--assets is the folder that holds MCLA-Setup.exe and the three archives (they are not copied); --out is
where the four files above are written. The archives are read, not trusted: the SDK bundle's manifest and FidelityFX
record are read from inside it and the checks of the build are repeated here (the bundle is the one
the verify job tested; its name matches its manifest; the FidelityFX DLLs have no C runtime import;
the source archive is of the same SDK commit and patches).
"""
import argparse
import json
import pathlib
import re
import sys
import tarfile
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cilib  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--tag", required=True)
ap.add_argument("--commit", required=True)
ap.add_argument("--repository", required=True)
ap.add_argument("--assets", required=True)
ap.add_argument("--launcher-zip", required=True)
ap.add_argument("--installer", required=True)
ap.add_argument("--sdk-bundle-zip", required=True)
ap.add_argument("--sdk-source-archive", required=True)
ap.add_argument("--launcher-notices", required=True)
ap.add_argument("--expect-bundle-sha256", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--run-url", default="")
a = ap.parse_args()

repo = pathlib.Path(__file__).resolve().parents[2]
assets = pathlib.Path(a.assets)
out = pathlib.Path(a.out)
out.mkdir(parents=True, exist_ok=True)
launcher_zip = assets / a.launcher_zip
installer_exe = assets / a.installer
bundle_zip = assets / a.sdk_bundle_zip
source_archive = assets / a.sdk_source_archive
for f in (launcher_zip, installer_exe, bundle_zip, source_archive):
    if not f.is_file():
        sys.exit(f"assemble-release: {f} is not there")
if not re.fullmatch(r"[0-9a-f]{40}", a.commit):
    sys.exit("assemble-release: --commit is not a full commit hash")
if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,62}", a.tag):
    sys.exit("assemble-release: --tag has characters other than letters, digits, . _ - (or starts with one of them)")


def describe(path):
    return {"asset": path.name, "sha256": cilib.sha256_file(path), "size": path.stat().st_size}


# --- the SDK bundle, read from inside
bundle = describe(bundle_zip)
if bundle["sha256"] != a.expect_bundle_sha256.lower():
    sys.exit(f"assemble-release: the bundle is {bundle['sha256']}, not {a.expect_bundle_sha256} (the one that was tested)")
with zipfile.ZipFile(bundle_zip) as z:
    manifest = json.loads(z.read("mcla-sdk-bundle.json").decode("utf-8-sig"))
    ffx = json.loads(z.read("fidelityfx-build.json").decode("utf-8-sig"))
    names = set(z.namelist())
    bundle_notice = z.read("NOTICE.txt").decode("utf-8")
for need in ("bin/rexglue.exe", "bin/rexruntime.dll", "bin/amd_fidelityfx_dx12.dll", "bin/amd_fidelityfx_vk.dll",
             "bin/libc++.dll", "bin/libunwind.dll", "lib/rexruntime.dll.a", "SOURCE.txt", "NOTICE.txt", "fork-patches/README.md"):
    if need not in names:
        sys.exit(f"assemble-release: the SDK bundle lacks {need}")
digest = cilib.patches_digest(manifest["patches"])
m = re.fullmatch(r"rexglue-sdk-([0-9a-f]{8})-([0-9a-f]{12})-llvm-mingw-(\d{8})\.zip", bundle_zip.name)
if not m or m.group(1) != manifest["sdkCommit"][:8] or m.group(2) != digest[:12]:
    sys.exit(f"assemble-release: the bundle's name {bundle_zip.name} does not say what its manifest says")
if manifest["toolchain"] != "mingw":
    sys.exit("assemble-release: the bundle is not a mingw one")
banned = re.compile(r"^(msvcp\d+(_\d+)?|vcruntime\d+(_\d+)?|concrt\d+|ucrtbase|api-ms-win-crt-.*)\.dll$", re.I)
if not ffx.get("staticCrt"):
    sys.exit("assemble-release: the FidelityFX DLLs were not built with the static C runtime")
if sorted(d["name"] for d in ffx["dlls"]) != ["amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll"]:
    sys.exit("assemble-release: the FidelityFX record does not list the two DLLs")
for dll in ffx["dlls"]:
    imports = [i.lower() for i in dll["imports"]]
    if "kernel32.dll" not in imports:
        sys.exit(f"assemble-release: {dll['name']} has no kernel32.dll import in the record: the import list was not read")
    bad = [i for i in imports if banned.match(i)]
    if bad:
        sys.exit(f"assemble-release: {dll['name']} imports the C runtime ({', '.join(bad)})")

# --- the source archive, read from inside
source = describe(source_archive)
with tarfile.open(source_archive, "r:xz") as t:
    first = t.next()
    top = first.name.split("/")[0]
    info = None
    for member in t:
        if member.name == f"{top}/SOURCE-INFO.json":
            info = json.loads(t.extractfile(member).read().decode("utf-8"))
            break
if info is None or source_archive.name != info["archive"]:
    sys.exit("assemble-release: the source archive has no SOURCE-INFO.json, or is named otherwise than it says")
if info["sdkCommit"] != manifest["sdkCommit"] or info["patchesDigest"] != digest or info["forkCommit"] != a.commit:
    sys.exit("assemble-release: the source archive is not of the SDK commit, the patches and the fork commit of this release")

# --- tools the wizard downloads
tools_all = json.loads((repo / "scripts" / "ci" / "tools.json").read_text(encoding="utf-8"))
tools = {}
for key in ("llvm-mingw", "cmake", "ninja", "mingit"):
    t = tools_all[key]
    tools[key] = {"version": t["version"], "url": t["url"], "sha256": t["sha256"], "size": t["size"]}

# --- the shader pack (fingerprints; in the repository)
packs = sorted((repo / "shaderpack").glob("*.shaderpack"))
shader_pack = None
if packs:
    p = packs[0]
    shader_pack = {"path": f"shaderpack/{p.name}", "sha256": cilib.sha256_file(p), "size": p.stat().st_size}

# --- the supported game's fingerprints (names, sizes, SHA-256 of its files; scripts/ci/make-game-files.py made
# the file from a disc once). Fingerprints only: nothing of the game.
game = json.loads((repo / "scripts" / "ci" / "game-files.json").read_text(encoding="utf-8"))
if game.get("format") != 1 or not game["files"] or any(not re.fullmatch(r"[0-9a-f]{64}", f["sha256"]) for f in game["files"]):
    sys.exit("assemble-release: scripts/ci/game-files.json is not a format 1 list of files with SHA-256")
if "default.xex" not in {f["path"] for f in game["files"]}:
    sys.exit("assemble-release: scripts/ci/game-files.json lists no default.xex")
game_field = {k: game[k] for k in ("title", "titleId", "xexVersion", "files")}

# --- MCLA-Setup.exe, read from inside: the installer program, then the payload (cilib.read_installer_header, the same
# reader scripts/ci/check-installer.py runs when the exe is built). The header is checked against the release.
installer_info = describe(installer_exe)
version = re.match(r"v(\d+\.\d+\.\d+)", a.tag)
with zipfile.ZipFile(launcher_zip) as z:
    launcher_zip_files = len(z.namelist())
try:
    payload_meta = cilib.check_installer_header(installer_exe, tag=a.tag, commit=a.commit,
                                                version=version.group(1) if version else "0.0.0", zip_files=launcher_zip_files)
except ValueError as e:
    sys.exit(f"assemble-release: MCLA-Setup.exe: {e}")

launcher_packages = json.loads((pathlib.Path(a.launcher_notices) / "packages.json").read_text(encoding="utf-8"))
with zipfile.ZipFile(launcher_zip) as z:
    source_commit = z.read("source-commit.txt").decode("ascii").strip()
    if "MclaLauncher.exe" not in z.namelist():
        sys.exit("assemble-release: the launcher zip has no MclaLauncher.exe")

release = {
    "schema": 1,
    "tag": a.tag,
    "commit": a.commit,
    "repository": a.repository,
    "workflowRun": a.run_url or None,
    "provenance": "GitHub build-provenance attestations for every asset: gh attestation verify <file> -R " + a.repository,
    "launcher": {
        **describe(launcher_zip),
        "sourceCommit": source_commit,
        "runtime": ".NET 10 and the Windows App SDK runtime, self-contained (nothing to install)",
        "packages": launcher_packages,
        "packagesNote": "NuGet packages float by design (Microsoft.UI.Reactor *-*, Microsoft.WindowsAppSDK.WinUI 2.*): these are the versions this release resolved",
    },
    "installer": {
        **installer_info,
        "version": payload_meta["version"],
        "launcherFiles": int(payload_meta["files"]),
        "launcherBytes": int(payload_meta["bytes"]),
        "usage": "MCLA-Setup.exe installs the launcher for the current user (no administrator rights) into %LOCALAPPDATA%\\Programs\\MCLA, makes shortcuts and starts it. "
                 "To update a launcher that it installed: download this asset, check its SHA-256, run it with: --update --wait-pid <the launcher's process id> --dir <the launcher's folder>",
        "unsigned": True,
    },
    "sdkBundle": {
        **bundle,
        "sdkCommit": manifest["sdkCommit"],
        "toolchain": manifest["toolchain"],
        "llvmMingw": m.group(3),
        "patchesDigest": digest,
        "patchCount": len(manifest["patches"]),
        "fidelityFx": {
            "commit": ffx["fidelityFxCommit"],
            "staticCrt": ffx["staticCrt"],
            "dlls": [{"name": d["name"], "sha256": d["sha256"], "size": d["size"]} for d in ffx["dlls"]],
        },
    },
    "sdkSource": {
        **source,
        "sdkCommit": info["sdkCommit"],
        "forkCommit": info["forkCommit"],
        "note": "the corresponding source of the SDK bundle's GPL and LGPL parts (SDK tree at the pinned commits, patched, and the fork)",
    },
    "tools": tools,
    "game": game_field,
    "shaderPack": shader_pack,
}

# --- notices
notices = [
    "Licences and notices of the Midnight Club: Los Angeles (static recompilation) release " + a.tag,
    "=" * 78,
    "",
    "This release holds no game data. It holds:",
    "",
    "  MCLA-Setup.exe: the installer program (MIT, launcher/installer) with the launcher folder inside it, the",
    f"  same files as {launcher_zip.name}: the launcher, MIT (launcher/LICENSE), with the .NET runtime and the",
    "  Windows App SDK runtime it needs. Its folder has the same notices in notices/.",
    f"  {bundle_zip.name}: the ReXGlue SDK built with the fork's patches, with llvm-mingw's C++ runtime and",
    "  AMD's FidelityFX (FSR 3.1) runtimes. Its NOTICE.txt, SOURCE.txt and licenses/ are in it and below.",
    f"  {source_archive.name}: the corresponding source of the SDK bundle (the SDK tree at its pinned commits,",
    "  patched, and the fork's repository at the release commit), for its GPL and LGPL parts.",
    "",
    "The fork's own code is GPL-3.0-or-later (LICENSE in the repository). Source for everything is",
    f"public: https://github.com/{a.repository}/tree/{a.commit}",
    "",
    "Tools the launcher's installer downloads from their own projects (not part of this release):",
]
for key, t in tools_all.items():
    if key in tools:
        notices.append(f"  {key} {t['version']}: {t['license']}")
        notices.append(f"      {t['url']}")
notices += ["", "", "=" * 78, "The launcher's packages (THIRD-PARTY-NOTICES-LAUNCHER.txt)", "=" * 78, ""]
notices.append((pathlib.Path(a.launcher_notices) / "THIRD-PARTY-NOTICES-LAUNCHER.txt").read_text(encoding="utf-8"))
notices += ["", "=" * 78, "The SDK bundle (NOTICE.txt inside it)", "=" * 78, "", bundle_notice, ""]
notices += ["=" * 78, "Code ported from other projects into the fork (THIRD-PARTY-NOTICES.md)", "=" * 78, "",
            (repo / "THIRD-PARTY-NOTICES.md").read_text(encoding="utf-8")]
(out / "NOTICES.txt").write_text("\r\n".join(line.replace("\r\n", "\n").replace("\n", "\r\n") for line in notices), encoding="utf-8", newline="")

# --- release.json, then the sums
(out / "release.json").write_text(json.dumps(release, indent=2) + "\n", encoding="utf-8", newline="\n")
files = [installer_exe, launcher_zip, bundle_zip, source_archive, out / "release.json", out / "NOTICES.txt"]
(out / "SHA256SUMS").write_text("".join(f"{cilib.sha256_file(f)} *{f.name}\n" for f in files), encoding="ascii", newline="\n")


# --- the text of the draft
def mb(n):
    return f"{n / 1e6:.1f} MB"


body = f"""<!-- mcla-release-draft -->
Release `{a.tag}`, built from commit `{a.commit}`{(' by ' + a.run_url) if a.run_url else ''}.

| Asset | Size | SHA-256 |
|---|---|---|
| `MCLA-Setup.exe` | {mb(release['installer']['size'])} | `{release['installer']['sha256']}` |
| `{launcher_zip.name}` | {mb(release['launcher']['size'])} | `{release['launcher']['sha256']}` |
| `{bundle_zip.name}` | {mb(release['sdkBundle']['size'])} | `{release['sdkBundle']['sha256']}` |
| `{source_archive.name}` | {mb(release['sdkSource']['size'])} | `{release['sdkSource']['sha256']}` |
| `release.json` | {(out / 'release.json').stat().st_size / 1e3:.1f} KB | `{cilib.sha256_file(out / 'release.json')}` |
| `NOTICES.txt` | {(out / 'NOTICES.txt').stat().st_size / 1e3:.1f} KB | `{cilib.sha256_file(out / 'NOTICES.txt')}` |
| `SHA256SUMS` | | the sums of the six above |

`release.json` is what the launcher's installer reads; `NOTICES.txt` has the licences and notices of everything in the assets. Every asset has a build-provenance attestation (`gh attestation verify <file> -R {a.repository}`).

- **MCLA-Setup.exe**: the one file a player downloads. Run it: it installs the launcher for the current user (no administrator rights asked for) in `%LOCALAPPDATA%\\Programs\\MCLA`, makes a Start menu and a desktop shortcut and starts it; the launcher's setup wizard then builds the game from the player's own disc image. **Unsigned**, so Windows SmartScreen may warn (the README says how to go on). The launcher updates itself by downloading this asset (SHA-256 in `release.json`) and running it with `--update`.
- **Launcher zip**: the same launcher folder as a zip, for anyone who wants it without the installer: unzip and run `MclaLauncher.exe`; .NET and the Windows App SDK are inside. Unsigned.
- **SDK bundle**: the ReXGlue SDK ({manifest['sdkCommit'][:8]}) with the fork's {len(manifest['patches'])} patches (digest `{digest[:12]}`), built with llvm-mingw {m.group(3)}, and FSR's two runtimes (FidelityFX `{ffx['fidelityFxCommit'][:8]}`, built with the C runtime linked in, so no Visual C++ Redistributable is needed). `scripts/make_pc.ps1 -Toolchain mingw -SdkBundle <this zip> -SdkBundleSha256 <the hash>` uses it.
- **Source archive**: the corresponding source of the bundle's GPL and LGPL parts (the SDK tree at the pinned commits with the patches applied, and the fork at this commit).
- `game` in `release.json` holds the fingerprints of the supported game (names, sizes and SHA-256 of its files: no content) for the launcher's setup wizard.
- No game data is in any asset. The game is built on the player's PC, from their own disc.

When publishing: leave the Target as it is (`{a.commit[:8]}`), so that the tag is made on the commit this was built from. The tag must point at `{a.commit}`, which `release.json` names; the launcher's installer checks that.
"""
(out / "release-notes.md").write_text(body, encoding="utf-8", newline="\n")
print(f"assemble-release: release.json, NOTICES.txt, SHA256SUMS, release-notes.md in {out}")
print(json.dumps({k: release[k] for k in ("tag", "commit", "installer")} | {"game files": len(release["game"]["files"])}, indent=2))

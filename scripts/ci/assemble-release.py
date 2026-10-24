r"""Writes the files of a draft release that are not built, from the built ones:

  python assemble-release.py --tag TAG --commit SHA --repository OWNER/NAME --assets <folder>
         --launcher-zip <zip> --sdk-bundle-zip <zip> --launcher-notices <folder> --out <folder>
         [--run-url URL]

  release.json     the one file the wizard reads: what the release holds, with the SHA-256 and size
                   of each asset, the SDK bundle's commit/patches/toolchain, the tools to download
                   (URL and SHA-256 each), the shader pack. "game" and "installer" are null for now.
  NOTICES.txt      the licences and notices of what the release's assets hold
  SHA256SUMS       the SHA-256 of every asset (the zips, release.json, NOTICES.txt)
  release-notes.md the text of the draft release

--assets is the folder that holds the two zips (they are not copied); --out is where the four files
above are written. The zips are read, not trusted: the SDK bundle's manifest and FidelityFX record are
read from inside it and the checks of release.json (the name matches the manifest, the DLLs have no
C runtime import) are repeated here.
"""
import argparse
import hashlib
import json
import pathlib
import re
import sys
import zipfile

ap = argparse.ArgumentParser()
ap.add_argument("--tag", required=True)
ap.add_argument("--commit", required=True)
ap.add_argument("--repository", required=True)
ap.add_argument("--assets", required=True)
ap.add_argument("--launcher-zip", required=True)
ap.add_argument("--sdk-bundle-zip", required=True)
ap.add_argument("--launcher-notices", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--run-url", default="")
a = ap.parse_args()

repo = pathlib.Path(__file__).resolve().parents[2]
assets = pathlib.Path(a.assets)
out = pathlib.Path(a.out)
out.mkdir(parents=True, exist_ok=True)
launcher_zip = assets / a.launcher_zip
bundle_zip = assets / a.sdk_bundle_zip
for f in (launcher_zip, bundle_zip):
    if not f.is_file():
        sys.exit(f"assemble-release: {f} is not there")
if not re.fullmatch(r"[0-9a-f]{40}", a.commit):
    sys.exit("assemble-release: --commit is not a full commit hash")
if not re.fullmatch(r"[A-Za-z0-9._-]+", a.tag):
    sys.exit("assemble-release: --tag has characters other than letters, digits, . _ -")

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def describe(path):
    return {"asset": path.name, "sha256": sha256(path), "size": path.stat().st_size}

# --- the SDK bundle, read from inside
with zipfile.ZipFile(bundle_zip) as z:
    manifest = json.loads(z.read("mcla-sdk-bundle.json").decode("utf-8-sig"))
    ffx = json.loads(z.read("fidelityfx-build.json").decode("utf-8-sig"))
    names = set(z.namelist())
    bundle_notice = z.read("NOTICE.txt").decode("utf-8")
for need in ("bin/rexglue.exe", "bin/rexruntime.dll", "bin/amd_fidelityfx_dx12.dll", "bin/amd_fidelityfx_vk.dll",
             "bin/libc++.dll", "bin/libunwind.dll", "lib/rexruntime.dll.a", "SOURCE.txt", "NOTICE.txt"):
    if need not in names:
        sys.exit(f"assemble-release: the SDK bundle lacks {need}")
patch_lines = "".join(f"{p['name']} {p['sha256']}\n" for p in manifest["patches"])
patches_hash = hashlib.sha256(patch_lines.encode("ascii")).hexdigest()[:12]
m = re.fullmatch(r"rexglue-sdk-([0-9a-f]{8})-([0-9a-f]{12})-llvm-mingw-(\d{8})\.zip", bundle_zip.name)
if not m or m.group(1) != manifest["sdkCommit"][:8] or m.group(2) != patches_hash:
    sys.exit(f"assemble-release: the bundle's name {bundle_zip.name} does not say what its manifest says")
if manifest["toolchain"] != "mingw":
    sys.exit("assemble-release: the bundle is not a mingw one")
banned = re.compile(r"^(msvcp\d+(_\d+)?|vcruntime\d+(_\d+)?|concrt\d+|ucrtbase|api-ms-win-crt-.*)\.dll$", re.I)
for dll in ffx["dlls"]:
    bad = [i for i in dll["imports"] if banned.match(i)]
    if bad:
        sys.exit(f"assemble-release: {dll['name']} imports the C runtime ({', '.join(bad)})")
if not ffx.get("staticCrt"):
    sys.exit("assemble-release: the FidelityFX DLLs were not built with the static C runtime")

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
    shader_pack = {"path": f"shaderpack/{p.name}", "sha256": sha256(p), "size": p.stat().st_size}

launcher_packages = json.loads((pathlib.Path(a.launcher_notices) / "packages.json").read_text(encoding="utf-8"))
source_commit = None
# source-commit.txt is inside the launcher zip
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
    "launcher": {
        **describe(launcher_zip),
        "sourceCommit": source_commit,
        "runtime": ".NET 10 and the Windows App SDK runtime, self-contained (nothing to install)",
        "packages": launcher_packages,
    },
    "installer": None,
    "sdkBundle": {
        **describe(bundle_zip),
        "sdkCommit": manifest["sdkCommit"],
        "toolchain": manifest["toolchain"],
        "llvmMingw": m.group(3),
        "patchesHash": patches_hash,
        "patchCount": len(manifest["patches"]),
        "fidelityFx": {
            "commit": ffx["fidelityFxCommit"],
            "staticCrt": ffx["staticCrt"],
            "dlls": [{"name": d["name"], "sha256": d["sha256"], "size": d["size"]} for d in ffx["dlls"]],
        },
    },
    "tools": tools,
    "game": None,
    "shaderPack": shader_pack,
}

# --- notices
notices = [
    "Licences and notices of the Midnight Club: Los Angeles (static recompilation) release " + a.tag,
    "=" * 78,
    "",
    "This release holds no game data. It holds:",
    "",
    f"  {launcher_zip.name}: the launcher, MIT (launcher/LICENSE), with the .NET runtime and the",
    "  Windows App SDK runtime it needs. Its folder has the same notices in notices/.",
    f"  {bundle_zip.name}: the ReXGlue SDK built with the fork's patches, with llvm-mingw's C++ runtime and",
    "  AMD's FidelityFX (FSR 3.1) runtimes. Its NOTICE.txt, SOURCE.txt and licenses/ are in it and below.",
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
files = [launcher_zip, bundle_zip, out / "release.json", out / "NOTICES.txt"]
(out / "SHA256SUMS").write_text("".join(f"{sha256(f)} *{f.name}\n" for f in files), encoding="ascii", newline="\n")

# --- the text of the draft
def mb(n):
    return f"{n / 1e6:.1f} MB"
body = f"""**Draft** of release `{a.tag}`, built from commit `{a.commit}`{(' by ' + a.run_url) if a.run_url else ''}. It is not published.

| Asset | Size | SHA-256 |
|---|---|---|
| `{launcher_zip.name}` | {mb(release['launcher']['size'])} | `{release['launcher']['sha256']}` |
| `{bundle_zip.name}` | {mb(release['sdkBundle']['size'])} | `{release['sdkBundle']['sha256']}` |
| `release.json` | {(out / 'release.json').stat().st_size} bytes | what the launcher's installer reads |
| `NOTICES.txt` | {mb((out / 'NOTICES.txt').stat().st_size)} | licences and notices of everything in the assets |
| `SHA256SUMS` | | |

- **Launcher**: unzip and run `MclaLauncher.exe`; .NET and the Windows App SDK are inside. Unsigned.
- **SDK bundle**: the ReXGlue SDK ({manifest['sdkCommit'][:8]}) with the fork's {len(manifest['patches'])} patches (hash `{patches_hash}`), built with llvm-mingw {m.group(3)}, and FSR's two runtimes (FidelityFX `{ffx['fidelityFxCommit'][:8]}`, built with the C runtime linked in, so no Visual C++ Redistributable is needed). `scripts/make_pc.ps1 -Toolchain mingw -SdkBundle <this zip> -SdkBundleSha256 <the hash>` uses it.
- `MCLA-Setup.exe` is not in this release yet (`installer` is `null` in `release.json`), and `game` is `null`: the installer and the list of the game's files come with the setup wizard.
- No game data is in any asset. The game is built on the player's PC, from their own disc.
"""
(out / "release-notes.md").write_text(body, encoding="utf-8", newline="\n")
print(f"assemble-release: release.json, NOTICES.txt, SHA256SUMS, release-notes.md in {out}")
print(json.dumps({k: release[k] for k in ("tag", "commit", "installer", "game")}, indent=2))

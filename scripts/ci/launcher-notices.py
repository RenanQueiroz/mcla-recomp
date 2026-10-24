r"""The notices for a published launcher: python launcher-notices.py <publish folder> <notices folder> [--nuget <packages folder>]

Reads the launcher's MclaLauncher.deps.json (what the publish folder holds, down to the
.NET runtime pack), and for every NuGet package in it takes its name, version, licence and
copyright from the package's .nuspec in NuGet's cache, and copies the licence and notice
files from the package's root. Writes into <notices folder>:

  LAUNCHER-LICENSE.txt                 the launcher's own licence (launcher/LICENSE, MIT)
  THIRD-PARTY-NOTICES-LAUNCHER.txt     the packages, versions and licences, in a table
  packages.json                        the same, for release.json
  licenses/<package>/<file>            the licence and notice files the packages bring

Fails if a package of the publish has no licence information at all (so that a new
dependency is looked at before it is released).
"""
import json
import os
import pathlib
import re
import shutil
import sys
import xml.etree.ElementTree as ET

args = [a for a in sys.argv[1:] if not a.startswith("--")]
nuget = pathlib.Path(os.environ.get("NUGET_PACKAGES") or os.path.expanduser("~/.nuget/packages"))
if "--nuget" in sys.argv:
    nuget = pathlib.Path(sys.argv[sys.argv.index("--nuget") + 1])
    args = [a for a in args if a != str(nuget)]
if len(args) != 2:
    sys.exit(__doc__)
publish, out = pathlib.Path(args[0]), pathlib.Path(args[1])
repo = pathlib.Path(__file__).resolve().parents[2]

deps = json.loads((publish / "MclaLauncher.deps.json").read_text(encoding="utf-8-sig"))
libraries = deps.get("libraries", {})
packages = []
for key, lib in libraries.items():
    kind = lib.get("type")
    name, _, version = key.partition("/")
    if kind == "package":
        packages.append((name, version))
    elif kind == "runtimepack":
        # runtimepack.Microsoft.NETCore.App.Runtime.win-x64/10.0.13 is the package
        # Microsoft.NETCore.App.Runtime.win-x64 (the .NET runtime in the publish folder).
        packages.append((name.removeprefix("runtimepack."), version))

out.mkdir(parents=True, exist_ok=True)
licenses = out / "licenses"
licenses.mkdir(exist_ok=True)
shutil.copyfile(repo / "launcher" / "LICENSE", out / "LAUNCHER-LICENSE.txt")

keep = re.compile(r"^(licen[sc]e|licen[sc]es|notice|third[-_ ]?party[-_ ]?notices?|copying|copyright)(\.[A-Za-z]+)?$", re.I)
# Packages whose nupkg carries no licence: their project's licence, checked at the
# commit the package says it was built from (scripts/ci/licenses/).
overrides = {
    "MessageFormat": ("MIT (the project's LICENSE at the package's commit dae6f0b0, github.com/jeffijoe/messageformat.net)",
                      "MessageFormat-LICENSE.txt"),
}
rows, problems = [], []
for name, version in sorted(set(packages), key=lambda p: p[0].lower()):
    folder = nuget / name.lower() / version
    nuspec = folder / f"{name.lower()}.nuspec"
    if not nuspec.exists():
        problems.append(f"{name} {version}: not in NuGet's cache at {folder}")
        continue
    root = ET.parse(nuspec).getroot()
    ns = {"n": root.tag.partition("}")[0].strip("{")}
    def text(tag):
        e = root.find(f".//n:metadata/n:{tag}", ns)
        return (e.text or "").strip() if e is not None and e.text else ""
    lic_el = root.find(".//n:metadata/n:license", ns)
    licence = ""
    if lic_el is not None:
        licence = (lic_el.text or "").strip() if lic_el.get("type") == "expression" else f"see {lic_el.text.strip()}" if lic_el.text else ""
    licence = licence or text("licenseUrl")
    copies = []
    for f in sorted(folder.iterdir()):
        if f.is_file() and keep.match(f.name):
            target = licenses / name / f.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(f, target)
            copies.append(f.name)
    # A licence file named by the nuspec's <license type="file"> sits anywhere in the package.
    if lic_el is not None and lic_el.get("type") == "file" and lic_el.text:
        f = folder / lic_el.text.strip().replace("\\", "/")
        if f.is_file() and f.name not in copies:
            target = licenses / name / f.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(f, target)
            copies.append(f.name)
    if not licence and not copies and name in overrides:
        licence, file = overrides[name]
        target = licenses / name / "LICENSE.txt"
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repo / "scripts" / "ci" / "licenses" / file, target)
        copies.append("LICENSE.txt")
    if not licence and not copies:
        problems.append(f"{name} {version}: no licence expression, URL or file")
    rows.append((name, version, licence or "(see the files)", text("copyright") or text("authors"), text("projectUrl"), copies))

lines = [
    "Third-party components in the launcher (MCLA-Launcher-*-win-x64.zip)",
    "=" * 68,
    "",
    "The launcher itself is MIT-licensed (LAUNCHER-LICENSE.txt). This is what its folder holds",
    "besides its own code: the .NET runtime and the NuGet packages it was published with, with",
    "the licence each declares. The files each package brings as its licence or notice are in",
    "licenses/<package>/. Versions are the ones this build resolved.",
    "",
]
for name, version, licence, copyright, url, copies in rows:
    lines.append(f"{name} {version}")
    lines.append(f"    licence:   {licence}")
    if copyright:
        lines.append(f"    copyright: {copyright}")
    if url:
        lines.append(f"    project:   {url}")
    if copies:
        lines.append(f"    files:     licenses/{name}/{', '.join(copies)}")
    lines.append("")
(out / "THIRD-PARTY-NOTICES-LAUNCHER.txt").write_text("\n".join(lines), encoding="utf-8", newline="\r\n")

(out / "packages.json").write_text(
    json.dumps([{"name": r[0], "version": r[1], "license": r[2]} for r in rows], indent=2) + "\n", encoding="utf-8", newline="\n")

print(f"launcher-notices: {len(rows)} packages, {sum(len(r[5]) for r in rows)} licence/notice files copied")
for p in problems:
    print("ERROR:", p)
sys.exit(1 if problems else 0)

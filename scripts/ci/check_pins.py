"""Consistency checks that need no build: python check_pins.py [repository root]

- scripts/ci/tools.json pins the same llvm-mingw and Clang (version and SHA-256) as scripts/make_pc.ps1;
- every patches/rexglue-*.patch is applied by make_pc.ps1 (or is one of the Linux-only ones), and
  every patch make_pc.ps1 applies is in patches/ (a patch added to the folder and not to the
  script, or the other way round, is otherwise found only by a player's build);
- every patch make_pc.ps1 applies is in FORK.md's order of `git apply` lines, in the same order;
- the workflows pin every action by commit SHA, never use pull_request_target, default to read-only permissions;
- no tracked file looks like game data (a disc image, a .xex, an .rpf, a .bik, a key...) or is
  over 8 MB;
- scripts/ci/game-files.json (the fingerprints the setup wizard checks the player's game against) has the
  shape the wizard reads: the title id, a version, and files with a plain relative name, a size and a
  64-digit SHA-256, default.xex among them. (Only the shape: what it lists is a decision of the maintainer.)
"""
import json
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
errors = []

make_pc = (root / "scripts" / "make_pc.ps1").read_text(encoding="utf-8", errors="replace")
tools = json.loads((root / "scripts" / "ci" / "tools.json").read_text(encoding="utf-8"))

def pinned(name):
    m = re.search(r'^\s*\$' + name + r'\s*=\s*"([^"]+)"', make_pc, re.M)
    if not m:
        errors.append(f"make_pc.ps1 has no ${name}")
        return None
    return m.group(1)

for key, version_var, sha_var in (("llvm-mingw", "mingwVersion", "mingwSha256"), ("clang-msvc", "llvmVersion", "llvmSha256")):
    version, sha = pinned(version_var), pinned(sha_var)
    if version and tools[key]["version"] != version:
        errors.append(f"tools.json {key} version {tools[key]['version']} != make_pc.ps1 {version}")
    if sha and tools[key]["sha256"] != sha:
        errors.append(f"tools.json {key} sha256 {tools[key]['sha256']} != make_pc.ps1 {sha}")

base = ["rexglue-v0.10.0-mcla.patch", "rexglue-ffmpeg-ps5-config.patch", "rexglue-minmax-blend-factor.patch"]
later = re.findall(r'@\{\s*Patch\s*=\s*"([^"]+\.patch)"', make_pc)
applied = base + later
if len(set(applied)) != len(applied):
    errors.append("make_pc.ps1 names a patch twice: " + ", ".join(sorted(p for p in set(applied) if applied.count(p) > 1)))
in_folder = sorted(p.name for p in (root / "patches").glob("rexglue-*.patch"))
linux_only = {n for n in in_folder if n.startswith("rexglue-linux-")}
for name in in_folder:
    if name not in applied and name not in linux_only:
        errors.append(f"patches/{name} is not applied by make_pc.ps1 (add it to $laterPatches, in the order it goes on)")
for name in applied:
    if name not in in_folder:
        errors.append(f"make_pc.ps1 applies {name}, which is not in patches/")

fork = (root / "FORK.md").read_text(encoding="utf-8", errors="replace")
order = re.findall(r"git apply \.\./\.\./patches/(rexglue-[A-Za-z0-9._-]+\.patch)", fork)
order += re.findall(r"git -C thirdparty/FFmpeg apply \.\./\.\./\.\./\.\./patches/(rexglue-[A-Za-z0-9._-]+\.patch)", fork)
for name in later:
    if name not in order:
        errors.append(f"FORK.md's list of git apply lines lacks {name}")
fork_later = [n for n in re.findall(r"git apply \.\./\.\./patches/(rexglue-[A-Za-z0-9._-]+\.patch)", fork) if n in later]
if fork_later != [n for n in later if n in fork_later]:
    errors.append("FORK.md's git apply lines are not in the order make_pc.ps1 applies them")

# Nothing from a player's game may be in the repository (.gitignore keeps these out; this is the
# check that it stayed so): no disc image, executable, archive or media file of the game, and no
# big file that is not one of the known ones.
import subprocess
tracked = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True).stdout.decode("utf-8").split("\0")
tracked = [t for t in tracked if t]
forbidden = re.compile(r"\.(iso|xex|xexp|rpf|bik|pe|bin|god|key)$", re.I)
for t in tracked:
    if forbidden.search(t):
        errors.append(f"{t} is tracked: game data and keys never go into the repository")
    size = (root / t).stat().st_size if (root / t).is_file() else 0
    if size > 8_000_000:
        errors.append(f"{t} is {size / 1e6:.1f} MB: no tracked file is over 8 MB (is it game data?)")

# The workflows: every third-party action is pinned by a full commit SHA, no pull_request_target,
# and the default permissions are read-only.
for wf in sorted((root / ".github" / "workflows").glob("*.yml")):
    text = wf.read_text(encoding="utf-8")
    for m in re.finditer(r"^\s*-?\s*uses:\s*(\S+)", text, re.M):
        ref = m.group(1)
        if not ref.startswith("./") and not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_./-]+@[0-9a-f]{40}", ref):
            errors.append(f"{wf.name}: {ref} is not pinned by a full commit SHA")
    if re.search(r"^\s*pull_request_target\s*:", text, re.M):
        errors.append(f"{wf.name}: pull_request_target is not allowed")
    if not re.search(r"^permissions:\s*\n\s+contents:\s*read\s*$", text, re.M):
        errors.append(f"{wf.name}: the top-level permissions are not 'contents: read'")
    if re.search(r"secrets\.(?!GITHUB_TOKEN)", text):
        errors.append(f"{wf.name}: uses a secret other than GITHUB_TOKEN")

# The Python sources of the repository (the scripts, the launcher's tools) are valid Python, with no
# invalid escape sequences (warnings made errors).
import warnings
checked_py = 0
for t in tracked:
    if t.endswith(".py"):
        checked_py += 1
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("error")
                compile((root / t).read_text(encoding="utf-8"), t, "exec")
        except (SyntaxError, Warning) as e:
            errors.append(f"{t}: {e}")

game_files = json.loads((root / "scripts" / "ci" / "game-files.json").read_text(encoding="utf-8"))
if game_files.get("titleId") != "545407F8":
    errors.append("game-files.json: titleId is not 545407F8")
if not re.fullmatch(r"\d+\.\d+\.\d+\.\d+", str(game_files.get("xexVersion", ""))):
    errors.append("game-files.json: xexVersion is not a four-part version")
listed = game_files.get("files") or []
if not listed:
    errors.append("game-files.json has no files")
for entry in listed:
    name = str(entry.get("path", ""))
    if (not name or ".." in name or ":" in name or name.startswith(("/", "\\"))
            or not isinstance(entry.get("size"), int) or entry["size"] < 0
            or not re.fullmatch(r"[0-9a-f]{64}", str(entry.get("sha256", "")))):
        errors.append(f"game-files.json: a bad entry: {entry!r}")
if "default.xex" not in [str(e.get("path", "")).lower() for e in listed]:
    errors.append("game-files.json does not list default.xex")

for e in errors:
    print("ERROR:", e)
print(f"check_pins: {checked_py} Python sources compile; {len(applied)} patches applied by make_pc.ps1, {len(in_folder)} in patches/, "
      f"{len(tracked)} tracked files checked for game data, {len(errors)} problem(s)")
sys.exit(1 if errors else 0)

r"""Compile every host source of the game (src\*.cpp) with -fsyntax-only, with the project's own
flags, from CMake's compile_commands.json: python host_check.py <build folder> [-j N]

The build folder is configured against the SDK (llvm-mingw, the SDK's headers) with the stand-in
generated headers (host_stub.py) first on the include path; no game is needed. This covers the
host code's syntax and types against the SDK's headers. It does not cover the recompiled game
code (that comes from the player's xex), linking, or running.
"""
import concurrent.futures
import json
import os
import re
import subprocess
import sys

build = sys.argv[1]
jobs = int(sys.argv[sys.argv.index("-j") + 1]) if "-j" in sys.argv else (os.cpu_count() or 2)
entries = json.load(open(os.path.join(build, "compile_commands.json"), encoding="utf-8"))
src_marker = os.sep + "src" + os.sep
todo = []
for e in entries:
    file = os.path.normpath(e["file"])
    if src_marker not in file or not file.endswith(".cpp"):
        continue
    command = e["command"]
    command = re.sub(r" -o \S+", "", command)
    command = re.sub(r" -MD -MT \S+ -MF \S+", "", command)
    command, replaced = re.subn(r" -c ", " -fsyntax-only ", command, count=1)
    if not replaced:
        # Without it the command would compile for real (and write an object file): not a check.
        sys.exit(f"host_check: the compile command of {file} has no ' -c ' to make -fsyntax-only of: {command[:200]}")
    todo.append((os.path.relpath(file, os.path.dirname(os.path.dirname(file))), command, e.get("directory", build)))

if not todo:
    sys.exit("host_check: no host source in compile_commands.json")

def run(item):
    name, command, directory = item
    p = subprocess.run(command, cwd=directory, shell=True, capture_output=True, text=True)
    return name, p.returncode, p.stdout + p.stderr

failed = 0
with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
    for name, code, output in pool.map(run, todo):
        print(f"{'ok  ' if code == 0 else 'FAIL'} {name}", flush=True)
        if code != 0:
            failed += 1
            print(output[-4000:], flush=True)
print(f"host_check: {len(todo) - failed} of {len(todo)} host sources compile")
sys.exit(1 if failed else 0)

r"""Downloads the tools release.json names and checks them against scripts/ci/tools.json:
python verify_tools.py [tool names...]   (default: llvm-mingw cmake ninja mingit)

The wizard downloads these from their own projects' releases and checks the SHA-256 in release.json;
this checks, when a release is made, that the URL still serves the file the pins say (a project that
replaces a release's asset, or a typo in a pin, is found here and not by a player). HTTPS only.
"""
import hashlib
import json
import pathlib
import sys
import urllib.request

tools = json.loads((pathlib.Path(__file__).with_name("tools.json")).read_text(encoding="utf-8"))
names = sys.argv[1:] or ["llvm-mingw", "cmake", "ninja", "mingit"]
failed = 0
for name in names:
    t = tools[name]
    if not t["url"].startswith("https://"):
        sys.exit(f"verify_tools: {name}'s URL is not https")
    h = hashlib.sha256()
    size = 0
    with urllib.request.urlopen(t["url"], timeout=120) as response:
        while chunk := response.read(1 << 20):
            h.update(chunk)
            size += len(chunk)
    ok = h.hexdigest() == t["sha256"] and size == t["size"]
    failed += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {name} {t['version']}: {size} bytes, sha256 {h.hexdigest()}" + ("" if ok else f" (pinned {t['size']} bytes, {t['sha256']})"), flush=True)
sys.exit(1 if failed else 0)

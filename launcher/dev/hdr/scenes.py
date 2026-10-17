"""Per scene marker (dev pad 'log SCENE x'), the composite mode in effect
(from the 'HDR: frames are put together from' lines) and the UI fallbacks.
usage: scenes.py GAME_LOG"""
import re, sys
mode = "?"
modes_since = set()
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = re.search(r"HDR: frames are put together from (.*)$", line)
    if m:
        mode = m.group(1).strip()
        modes_since.add(mode)
        continue
    m = re.search(r"HDR: the UI layer fell back to the difference mask \((.*)\)", line)
    if m:
        print("   fallback:", m.group(1))
        continue
    m = re.search(r"dev pad:\s+SCENE (\S+)\s*$", line)
    if m and "-> ok" not in line:
        print(f"{m.group(1):18s} now: {mode:28s} seen since last: {', '.join(sorted(modes_since))}")
        modes_since = {mode}

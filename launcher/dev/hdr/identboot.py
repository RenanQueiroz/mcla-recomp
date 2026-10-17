"""Boot screens across two builds: for each run-A shot with content, whether
some run-B shot is byte-identical.  usage: identboot.py DIR tagA tagB"""
import glob, os, sys, hashlib
import numpy as np
from PIL import Image
d, ta, tb = sys.argv[1:4]
def load(t):
    out = []
    for f in sorted(glob.glob(os.path.join(d, f"{t}_boot*.png"))):
        a = np.asarray(Image.open(f).convert("RGB"))
        out.append((os.path.basename(f), a, hashlib.sha1(a.tobytes()).hexdigest(), a.max() > 8))
    return out
A, Bs = load(ta), load(tb)
hb = {h for _, _, h, _ in Bs}
content = [x for x in A if x[3]]
matched = [x for x in content if x[2] in hb]
distinct = {x[2] for x in content}
distinct_matched = {x[2] for x in matched}
print(f"{ta} vs {tb}: {len(content)} shots with content ({len(distinct)} distinct frames); "
      f"{len(matched)} have a byte-identical shot in {tb} ({len(distinct_matched)} distinct)")

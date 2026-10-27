r"""Checks the smoke run of the audio work (batch S): python -P -E check_smoke.py <runs folder> <dumps folder>
Exit 0 = the game ran, the statistics lines and the dumps are there."""
import json
import sys
from pathlib import Path

runs, dumps = Path(sys.argv[1]), Path(sys.argv[2])
problems = []
res = runs / "S" / "results.jsonl"
rows = [json.loads(line) for line in res.read_text(encoding="utf-8").splitlines() if line.strip()] if res.exists() else []
if not rows:
    problems.append("no result row for batch S")
for r in rows:
    if r.get("error"):
        problems.append("run error: " + str(r["error"])[:300])
log = runs / "S" / "smoke" / "game.log"
text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
if not text:
    problems.append("no game.log")
for needle in ("audio dump: writing to", "audio stats TOTAL", "audio stats interval"):
    if needle not in text:
        problems.append(f"game.log lacks '{needle}'")
d = dumps / "S-smoke"
mix = d / "mix.wav"
if not mix.exists() or mix.stat().st_size < 100_000:
    problems.append("mix.wav missing or tiny")
ctx = sorted(d.glob("xma_ctx*_s*.wav")) if d.exists() else []
if not ctx:
    problems.append("no xma_ctx*.wav (no XMA context decoded anything)")
print("smoke: mix.wav", mix.stat().st_size if mix.exists() else 0, "bytes;", len(ctx), "context streams;",
      sum(p.stat().st_size for p in ctx), "bytes of decoded PCM")
for line in text.splitlines():
    if "audio stats TOTAL" in line:
        print(line[-600:])
for p in problems:
    print("PROBLEM:", p)
print("SMOKE", "FAIL" if problems else "PASS")
sys.exit(1 if problems else 0)

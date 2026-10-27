r"""Checks the smoke run of the audio work (batch S): python -P -E check_smoke.py <runs folder> <dumps folder>
Exit 0 = the game ran and EXITED WITH CODE 0, the statistics lines (the cumulative TOTAL too) and the dumps are there, the dump files
are complete WAVs (header sizes patched: the game leaves through std::_Exit, so this is what the exit-time flush must have done) and the
exit-time totals file exists."""
import json
import re
import sys
from pathlib import Path

runs, dumps = Path(sys.argv[1]), Path(sys.argv[2])
problems = []
res = runs / "S" / "results.jsonl"
rows = [json.loads(line) for line in res.read_text(encoding="utf-8").splitlines() if line.strip()] if res.exists() else []
if not rows:
    problems.append("no result row for batch S")
for r in rows[-1:]:
    if r.get("error"):
        problems.append("run error: " + str(r["error"])[:300])
    code = r.get("exit_code")
    if code is None:
        problems.append("the game's exit code was not recorded")
    elif code != 0:
        problems.append(f"the game exited with code {code:#x}, not 0")
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


def wav_problem(p):
    """None when the file is a complete WAV written by the patch's writer (44-byte header, sizes patched), else why not."""
    raw = p.read_bytes()
    if len(raw) < 44 or raw[:4] != b"RIFF" or raw[8:16] != b"WAVEfmt " or raw[36:40] != b"data":
        return "not a WAV with the writer's 44-byte header"
    riff = int.from_bytes(raw[4:8], "little")
    data = int.from_bytes(raw[40:44], "little")
    if riff != len(raw) - 8 or data != len(raw) - 44:
        return f"header sizes riff={riff} data={data} do not match the file ({len(raw)} bytes)"
    return None


bad = []
for p in ([mix] if mix.exists() else []) + ctx:
    why = wav_problem(p)
    if why:
        bad.append(f"{p.name}: {why}")
if bad:
    problems.append(f"{len(bad)} dump WAV(s) incomplete, e.g. {bad[0]}")
total_file = d / "audio_stats_total.txt"
if not total_file.exists():
    problems.append("audio_stats_total.txt is missing (the exit-time guard did not run)")
else:
    line = total_file.read_text(encoding="utf-8", errors="replace").strip()
    if "audio stats TOTAL" not in line:
        problems.append("audio_stats_total.txt has no TOTAL line")
    m = re.search(r"t=([\d.]+)s", line)
    if m:
        print(f"smoke: audio_stats_total.txt written at t={m.group(1)} s")
jsons = list(d.glob("xma_ctx*_s*.json")) if d.exists() else []
if len(jsons) < len(ctx):
    problems.append(f"{len(ctx) - len(jsons)} context .wav without a .json")
print("smoke: mix.wav", mix.stat().st_size if mix.exists() else 0, "bytes;", len(ctx), "context streams;",
      sum(p.stat().st_size for p in ctx), "bytes of decoded PCM;", len(jsons), "sidecars; exit code",
      rows[-1].get("exit_code") if rows else None)
last = None
for line in text.splitlines():
    if "audio stats TOTAL" in line:
        last = line
if last:
    print(last[-600:])
for p in problems:
    print("PROBLEM:", p)
print("SMOKE", "FAIL" if problems else "PASS")
sys.exit(1 if problems else 0)

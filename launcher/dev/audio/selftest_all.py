r"""Self-test of analyze_all.py on a synthetic run tree (no game data). python -P -E selftest_all.py"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import selftest_analyze as st  # noqa: E402


def stats_line(label, t, underruns, wrote, worker):
    return (f"[2026-10-27 12:00:{int(t):02d}.000] [info] [apu] [t5] audio stats {label} t={t:.1f}s | xma: kicks=100 work inline=90 worker={worker} "
            f"avg_us=300 max_us=2500 slow(>2ms)=1 frames=5000 errors=0 recheck_abort=0 stalled=0 cas_retry=0 | game wrote during a decode: "
            f"read_offset={wrote} valid=3 other=1 | pump: n=900 avg_us=400 max_us=3000 gap_max_us=9000 late(>8ms)=2 | "
            f"sdl: callbacks=500 frames=900 underruns={underruns} queue_min=2")


def main():
    tmp = Path(tempfile.mkdtemp(prefix="audio_selftest_all_"))
    runs, dumps, out = tmp / "runs", tmp / "dumps", tmp / "out"
    rate = 48000
    n = rate * 40
    for batch, tag, variant, underruns, wrote in (("A", "old1", "old", 4, 7), ("A", "new1", "new", 1, 7)):
        folder = runs / batch / tag
        folder.mkdir(parents=True)
        d = dumps / f"{batch}-{tag}"
        d.mkdir(parents=True)
        m = np.stack([st.music(n, rate, 1), st.music(n, rate, 2)], axis=1).astype(np.float32)
        for g in range(underruns):
            m[(3 + g * 7) * rate:(3 + g * 7) * rate + 300, :] = 0.0
        st.write_wav(d / "mix.wav", m, rate, "f32")
        c = np.stack([st.music(n, rate, 3), st.music(n, rate, 4)], axis=1)
        st.write_wav(d / "xma_ctx9_s0.wav", c.astype(np.float32), rate, "i16")
        log = [
            "[2026-10-27 12:00:00.000] [info] [apu] [t5] audio dump: writing to C:/x/ (at most 1500 MB)",
            "[2026-10-27 12:00:08.000] [info] [core] [t9] dev pad: mark title at engine frame 0, swap 0",
            "[2026-10-27 12:00:25.000] [info] [core] [t9] dev pad: mark drive at engine frame 900, swap 900",
            stats_line("interval", 5.0, 1, 2, 0), stats_line("TOTAL", 40.0, underruns, wrote, 0),
        ]
        (folder / "game.log").write_text("\n".join(log) + "\n", encoding="utf-8")
        (runs / batch / "results.jsonl").open("a", encoding="utf-8").write(json.dumps(
            {"tag": f"{batch}-{tag}", "cfg": {"tag": tag, "variant": variant, "aff": "0xFFF"}, "seconds": 40}) + "\n")
    r = subprocess.run([sys.executable, "-P", "-E", str(HERE / "analyze_all.py"), "--runs", str(runs), "--dumps", str(dumps), "--out", str(out),
                        "--batches", "A", "--clips", str(tmp / "clips")], capture_output=True, text=True)
    print(r.stdout[-3500:])
    print(r.stderr[-2000:])
    ok = r.returncode == 0 and (out / "SUMMARY.md").exists() and (tmp / "clips" / "old-radio-old1.wav").exists()
    ok = ok and "No regression in `new` on these measures: YES" in (out / "SUMMARY.md").read_text(encoding="utf-8")
    print("SELFTEST_ALL", "PASS" if ok else "FAIL", tmp)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

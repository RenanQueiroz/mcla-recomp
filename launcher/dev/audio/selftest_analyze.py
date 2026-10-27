r"""Self-test of audio_analyze.py on synthetic audio (no game data): a mix with known gaps and clicks, a stereo context with a
known channel dropout and drifting balance. python -I selftest_analyze.py"""
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import audio_analyze as aa  # noqa: E402


def write_wav(path, x, rate, kind):
    ch = x.shape[1]
    if kind == "f32":
        data = x.astype("<f4").tobytes()
        tag, bits = 3, 32
    else:
        data = np.clip(x * 32768, -32768, 32767).astype("<i2").tobytes()
        tag, bits = 1, 16
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, tag, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits)
    Path(path).write_bytes(hdr + b"data" + struct.pack("<I", len(data)) + data)


def music(n, rate, seed):
    rng = np.random.default_rng(seed)
    t = np.arange(n) / rate
    x = 0.12 * np.sin(2 * np.pi * 220 * t) + 0.08 * np.sin(2 * np.pi * 1370 * t + 1) + 0.05 * rng.standard_normal(n)
    # a slow amplitude wobble, like music
    return x * (0.7 + 0.3 * np.sin(2 * np.pi * 0.7 * t))


def main():
    rate = 48000
    n = rate * 30
    tmp = Path(tempfile.mkdtemp(prefix="audio_selftest_"))
    # mix: 30 s stereo float, three gaps of 400 frames, two clicks
    m = np.stack([music(n, rate, 1), music(n, rate, 2)], axis=1).astype(np.float32)
    gaps_at = [5 * rate + 100, 12 * rate + 7, 22 * rate + 3000]
    for g in gaps_at:
        m[g:g + 400, :] = 0.0
    clicks_at = [8 * rate + 555, 17 * rate + 1234]
    for c in clicks_at:
        m[c:c + 3, 0] += 0.6
    write_wav(tmp / "mix.wav", m, rate, "f32")
    # a radio-like context: 30 s stereo 16-bit; left silent from 10 s to 14 s; right level drifting 0 -> -12 dB
    c = np.stack([music(n, rate, 3), music(n, rate, 4)], axis=1)
    c[10 * rate:14 * rate, 0] = 0.0
    c[:, 1] *= np.linspace(1.0, 0.25, n)
    write_wav(tmp / "xma_ctx7_s0.wav", c.astype(np.float32), rate, "i16")
    rep = aa.analyze_mix(tmp / "mix.wav")
    ctx = aa.analyze_ctx(tmp / "xma_ctx7_s0.wav")
    print(json.dumps(rep, indent=1))
    print(json.dumps(ctx, indent=1))
    ok = True

    def check(label, cond):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + label)
        ok &= bool(cond)

    check("three gaps found", rep["gaps"] == 3)
    check("gap length 400 frames = 8.3 ms", all(abs(rep["gap_ms_longest"] - 8.3) < 0.5 for _ in [0]))
    check("two clicks found", rep["clicks"] == 2)
    check("no balance dropouts in the mix", rep["balance"]["dropout_windows"] == 0)
    check("context dropout windows == 4 (the 4 s the left channel is silent)", 3 <= ctx["balance"]["dropout_windows"] <= 5)
    check("context balance moves (std > 1 dB)", ctx["balance"].get("balance_db_std", 0) > 1.0)
    # the reference path: a container ffmpeg accepts
    import xma2_riff
    riff = tmp / "x.wav"
    riff.write_bytes(xma2_riff.build(b"\x00" * 2048 * 4, 48000, 2))
    r = subprocess.run([aa.FFMPEG, "-hide_banner", "-i", str(riff), "-f", "null", "-"], capture_output=True, text=True)
    check("ffmpeg reads the XMA2 container header (decoder xma2)", "xma2" in (r.stderr or ""))
    # alignment: a delayed copy
    a = np.stack([music(n, rate, 9), music(n, rate, 9)], axis=1).astype(np.float32)
    b = np.concatenate([np.zeros((777, 2), np.float32), a])
    lag = aa.align(a, b)
    check(f"align finds the lag 777 (got {lag})", lag == 777)
    print("SELFTEST", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

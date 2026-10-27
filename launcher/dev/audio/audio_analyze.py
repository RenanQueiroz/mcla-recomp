r"""Analysis of one audio run: the dumps the SDK writes with --audio_dump_dir, and the statistics lines of game.log.

   python -I audio_analyze.py <run name> --dump <dump dir> [--log <game.log>] [--out <report.json>] [--ref]

What it measures (nothing here needs listening):
 * mix.wav (what the output device was given; 32-bit float, 2 or 6 channels):
     gaps       runs of exact digital silence (all channels 0.0) of at least 64 sample frames inside playing
                audio: the SDL callback finding no frame queued (an underrun puts silence in);
     clicks     isolated spikes of the second difference of the front left/right channels, far above the local
                level of the same quantity (a splice, a dropped or repeated block);
     balance    left/right level per second (dB, only where both are playing): its spread over the run.
 * xma_ctx*_s*.wav (what one XMA context decoded; 16-bit): the same level, balance and click measures, a
   "channel dropout" count (one channel silent while the other plays), and with --ref the comparison with an
   independent decode (FFmpeg's xma2 decoder on the .xma packets that context was given).
 * game.log: the "audio stats" lines (xma, pump and sdl counters), the underrun lines and the dev pad marks.
"""
import argparse
import json
import math
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

FFMPEG = "ffmpeg"


# ---------------------------------------------------------------------------------------------------------------------
def read_wav(path):
    """(rate, channels, float32 array [frames, channels] in -1..1 (float files as they are)). Tolerates a header whose sizes were
    never patched (a killed process): the data chunk then runs to the end of the file."""
    raw = Path(path).read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a WAV")
    pos = 12
    fmt = None
    data = None
    while pos + 8 <= len(raw):
        tag = raw[pos:pos + 4]
        size = int.from_bytes(raw[pos + 4:pos + 8], "little")
        body = pos + 8
        if tag == b"fmt ":
            fmt = raw[body:body + size]
        elif tag == b"data":
            end = body + size
            if size == 0 or end > len(raw):
                end = len(raw)
            data = raw[body:end]
            break
        pos = body + size + (size & 1)
    if fmt is None or data is None:
        raise ValueError(f"{path}: no fmt or data chunk")
    tag = int.from_bytes(fmt[0:2], "little")
    channels = int.from_bytes(fmt[2:4], "little")
    rate = int.from_bytes(fmt[4:8], "little")
    bits = int.from_bytes(fmt[14:16], "little")
    if tag == 3 and bits == 32:
        arr = np.frombuffer(data[: len(data) // 4 * 4], dtype="<f4")
    elif tag == 1 and bits == 16:
        arr = np.frombuffer(data[: len(data) // 2 * 2], dtype="<i2").astype(np.float32) / 32768.0
    else:
        raise ValueError(f"{path}: unsupported format tag {tag}, {bits} bits")
    frames = len(arr) // channels
    return rate, channels, arr[: frames * channels].reshape(frames, channels).astype(np.float32, copy=False)


def db(x, floor=-120.0):
    return floor if x <= 1e-6 else max(floor, 20.0 * math.log10(x))


def per_second_rms(x, rate):
    """[seconds, channels] RMS of whole seconds (the last partial second is dropped)."""
    n = len(x) // rate
    if n == 0:
        return np.zeros((0, x.shape[1]))
    blocks = x[: n * rate].reshape(n, rate, x.shape[1])
    return np.sqrt((blocks.astype(np.float64) ** 2).mean(axis=1))


def balance_stats(x, rate, window_s=1):
    """Left/right balance in dB per window where both channels play (above -50 dBFS), and the dropouts: windows in which one
    channel is below -75 dBFS while the other is above -35."""
    if x.shape[1] < 2:
        return {"windows": 0}
    win = rate * window_s
    n = len(x) // win
    if n == 0:
        return {"windows": 0}
    blocks = x[: n * win, :2].reshape(n, win, 2).astype(np.float64)
    rms = np.sqrt((blocks ** 2).mean(axis=1))
    both = (rms[:, 0] > 10 ** (-50 / 20)) & (rms[:, 1] > 10 ** (-50 / 20))
    bal = 20 * np.log10(np.maximum(rms[:, 0], 1e-9) / np.maximum(rms[:, 1], 1e-9))
    dropouts = ((rms[:, 0] < 10 ** (-75 / 20)) & (rms[:, 1] > 10 ** (-35 / 20))) | \
               ((rms[:, 1] < 10 ** (-75 / 20)) & (rms[:, 0] > 10 ** (-35 / 20)))
    out = {"windows": int(n), "windows_both_playing": int(both.sum()), "dropout_windows": int(dropouts.sum())}
    if both.any():
        b = bal[both]
        out.update(balance_db_median=round(float(np.median(b)), 2), balance_db_p5=round(float(np.percentile(b, 5)), 2),
                   balance_db_p95=round(float(np.percentile(b, 95)), 2), balance_db_std=round(float(b.std()), 2),
                   balance_db_max_abs=round(float(np.abs(b).max()), 2))
        if len(b) > 2:
            # How much the balance moves from one window to the next: the "goes wild" measure
            out["balance_step_db_p95"] = round(float(np.percentile(np.abs(np.diff(b)), 95)), 2)
    return out


def balance_series(x, rate, window_s=5):
    """Left/right balance in dB (20 log10 rmsL/rmsR) per window of window_s seconds; None where either channel is below -50 dBFS
    (nothing to balance). Used to see the balance over time (a stream whose balance wanders or flips)."""
    if x.shape[1] < 2:
        return []
    win = int(rate * window_s)
    n = len(x) // win
    if n == 0:
        return []
    blocks = x[: n * win, :2].reshape(n, win, 2).astype(np.float64)
    rms = np.sqrt((blocks ** 2).mean(axis=1))
    floor = 10 ** (-50 / 20)
    out = []
    for l, r in rms:
        out.append(round(float(20 * np.log10(l / r)), 2) if l > floor and r > floor else None)
    return out


def balance_trend(series):
    """{"slope_db_per_min", "drift_db"} of a balance series (5 s windows): the least-squares slope, and the median of the last third
    minus the median of the first third. Needs 6 windows with both channels playing."""
    pts = [(i, v) for i, v in enumerate(series) if v is not None]
    if len(pts) < 6:
        return {}
    t = np.array([p[0] for p in pts], dtype=np.float64) * 5.0 / 60.0
    v = np.array([p[1] for p in pts], dtype=np.float64)
    slope = float(np.polyfit(t, v, 1)[0])
    k = max(1, len(v) // 3)
    return {"slope_db_per_min": round(slope, 2), "drift_db": round(float(np.median(v[-k:]) - np.median(v[:k])), 2)}


def find_clicks(x, rate, order=12, block=4096, sigma_factor=9.0, floor=0.03):
    """Isolated discontinuities: a sample whose linear-prediction residual is far above the block's (robust sigma) while the
    residual 8..48 samples later is back to normal. A splice, a dropped or repeated block, a one-sample glitch look like that;
    a drum onset (the residual stays high after it) and ordinary content do not. Order-12 LPC per 4096-frame block on the
    sum of the first two channels. Returns the positions in frames."""
    sig = x[:, :2].astype(np.float64).sum(axis=1) if x.shape[1] >= 2 else x[:, 0].astype(np.float64)
    n = len(sig)
    clicks = []
    last = -10 ** 9
    for b0 in range(0, n - block - order, block):
        seg = sig[b0:b0 + block + order]
        if np.abs(seg).max() < 1e-4:
            continue
        w = seg[order:block + order]
        # autocorrelation method on the block
        r = np.array([np.dot(w[: block - k], w[k:]) for k in range(order + 1)])
        r[0] = r[0] * 1.0000001 + 1e-9
        try:
            toep = r[np.abs(np.subtract.outer(np.arange(order), np.arange(order)))]
            a = np.linalg.solve(toep, r[1:order + 1])
        except np.linalg.LinAlgError:
            continue
        # residual of the whole block (prediction from the `order` previous samples)
        pred = np.zeros(block)
        for k in range(1, order + 1):
            pred += a[k - 1] * seg[order - k: order - k + block]
        res = seg[order:order + block] - pred
        sigma = max(np.median(np.abs(res)) / 0.6745, 1e-5)
        thr = max(sigma_factor * sigma, floor)
        cand = np.nonzero(np.abs(res) > thr)[0]
        for p in cand:
            pos = b0 + int(p)
            if pos - last <= 64:
                last = pos
                continue
            after = res[p + 8:p + 48]
            if len(after) and np.abs(after).max() < 4.0 * sigma + 1e-9:
                clicks.append(pos)
                last = pos
    return clicks


def find_gaps(x, min_frames=64, context=2048):
    """Runs of all-zero frames of at least min_frames that have playing audio on both sides within `context` frames."""
    zero = np.all(x == 0.0, axis=1)
    if not zero.any():
        return []
    edges = np.diff(np.concatenate(([0], zero.astype(np.int8), [0])))
    starts = np.nonzero(edges == 1)[0]
    ends = np.nonzero(edges == -1)[0]
    gaps = []
    for s, e in zip(starts, ends):
        if e - s < min_frames:
            continue
        before = x[max(0, s - context):s]
        after = x[e:e + context]
        if len(before) == 0 or len(after) == 0:
            continue
        if np.abs(before).max() > 1e-4 and np.abs(after).max() > 1e-4:
            gaps.append((int(s), int(e - s)))
    return gaps


def analyze_mix(path):
    rate, ch, x = read_wav(path)
    seconds = len(x) / rate
    out = {"file": Path(path).name, "rate": rate, "channels": ch, "seconds": round(seconds, 2)}
    if len(x) < rate:
        return out
    gaps = find_gaps(x)
    out["gaps"] = len(gaps)
    out["gaps_per_min"] = round(len(gaps) / seconds * 60, 2) if seconds else 0
    out["gap_ms_total"] = round(sum(g[1] for g in gaps) / rate * 1000, 1)
    out["gap_ms_longest"] = round(max((g[1] for g in gaps), default=0) / rate * 1000, 1)
    out["gap_times_s"] = [round(g[0] / rate, 2) for g in gaps[:40]]
    clicks = find_clicks(x, rate)
    out["clicks"] = len(clicks)
    out["clicks_per_min"] = round(len(clicks) / seconds * 60, 2) if seconds else 0
    out["click_times_s"] = [round(c / rate, 2) for c in clicks[:40]]
    out["balance"] = balance_stats(x, rate)
    out["balance_series_5s"] = balance_series(x, rate)
    out["balance_trend"] = balance_trend(out["balance_series_5s"])
    r = per_second_rms(x, rate)
    if len(r):
        loud = r[:, :2].max(axis=1)
        out["level_db_median"] = round(db(float(np.median(loud))), 1)
        out["silent_seconds"] = int((loud < 10 ** (-70 / 20)).sum())
    out["peak"] = round(float(np.abs(x[:, :2]).max()), 3)
    out["clipped_frames"] = int((np.abs(x[:, :2]).max(axis=1) >= 0.999).sum())
    return out


def analyze_ctx(path):
    rate, ch, x = read_wav(path)
    seconds = len(x) / rate
    out = {"file": Path(path).name, "rate": rate, "channels": ch, "seconds": round(seconds, 2)}
    if len(x) < rate // 2:
        return out
    clicks = find_clicks(x, rate)
    out["clicks"] = len(clicks)
    out["clicks_per_min"] = round(len(clicks) / seconds * 60, 2) if seconds else 0
    out["click_times_s"] = [round(c / rate, 2) for c in clicks[:20]]
    out["balance"] = balance_stats(x, rate)
    out["balance_series_5s"] = balance_series(x, rate)
    out["balance_trend"] = balance_trend(out["balance_series_5s"])
    out["peak"] = round(float(np.abs(x).max()), 3)
    side = Path(path).with_suffix(".json")
    if side.exists():
        # The context's sidecar (written with every block, so it is there for a stream still open when the game left): how much audio the
        # decoder produced against the wall time between the first and the last write. About 1.0 when the game consumes the stream in real
        # time; the decoder runs ahead by a buffer only, so a stream consumed at another speed (the game's audio clock against the wall
        # clock) shows as a ratio that is not 1.
        try:
            side_j = json.loads(side.read_text(encoding="utf-8"))
            wall = (side_j["last_us"] - side_j["first_us"]) / 1e6
            dec = side_j["pcm_bytes"] / (side_j["rate"] * side_j["channels"] * 2)
            out["decoded_s"] = round(dec, 2)
            out["wall_s"] = round(wall, 2)
            out["decode_vs_wall"] = round(dec / wall, 4) if wall > 5 else None
            out["first_us"] = side_j["first_us"]
        except (ValueError, KeyError, ZeroDivisionError):
            pass
    return out


# ---------------------------------------------------------------------------------------------------------------------
def ffmpeg_decode(riff_path, out_pcm):
    cmd = [FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-i", str(riff_path), "-f", "s16le", "-acodec", "pcm_s16le", str(out_pcm)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, (r.stderr or "")[-1500:]


def align(a, b, max_lag=24000, span=480000):
    """Lag (in frames) such that a[i] ~ b[i + lag], by FFT cross-correlation of the mono mixes of the first `span` frames."""
    am = a[:span].astype(np.float64).mean(axis=1)
    bm = b[:span + max_lag].astype(np.float64).mean(axis=1)
    n = 1
    while n < len(am) + len(bm):
        n *= 2
    fa = np.fft.rfft(am, n)
    fb = np.fft.rfft(bm, n)
    cc = np.fft.irfft(fb * np.conj(fa), n)
    lags = np.concatenate((np.arange(0, max_lag + 1), np.arange(-max_lag, 0)))
    cand = np.concatenate((cc[: max_lag + 1], cc[n - max_lag:n]))
    return int(lags[int(np.argmax(cand))])


def ref_compare(ctx_wav, tmp_dir):
    """Decodes the .xma of this context with FFmpeg's xma2 decoder and compares: returns the lag and the match statistics."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import xma2_riff
    stem = Path(ctx_wav).with_suffix("")
    xma = stem.with_suffix(".xma")
    meta_path = stem.with_suffix(".json")
    if not xma.exists():
        return {"error": "no .xma input dump"}
    meta = json.loads(meta_path.read_text()) if meta_path.exists() else {}
    rate, ch, ours = read_wav(ctx_wav)
    tmp_dir = Path(tmp_dir)
    tmp_dir.mkdir(parents=True, exist_ok=True)
    riff = tmp_dir / (stem.name + ".ref.wav")
    pcm = tmp_dir / (stem.name + ".ref.pcm")
    riff.write_bytes(xma2_riff.build(xma.read_bytes(), rate, ch))
    rc, err = ffmpeg_decode(riff, pcm)
    out = {"ffmpeg_rc": rc}
    if err.strip():
        out["ffmpeg_stderr"] = err.strip().splitlines()[-3:]
    if not pcm.exists() or pcm.stat().st_size < 4096:
        out["error"] = "the reference decode produced no usable output"
        return out
    ref = np.frombuffer(pcm.read_bytes(), dtype="<i2")
    ref = ref[: len(ref) // ch * ch].reshape(-1, ch).astype(np.float32) / 32768.0
    out["ours_seconds"] = round(len(ours) / rate, 2)
    out["ref_seconds"] = round(len(ref) / rate, 2)
    if len(ours) < 8192 or len(ref) < 8192:
        out["error"] = "too short to compare"
        return out
    lag = align(ours, ref)
    out["lag_frames"] = lag
    # compare the overlap
    if lag >= 0:
        a, b = ours[: len(ours)], ref[lag:]
    else:
        a, b = ours[-lag:], ref
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    diff = np.abs(a.astype(np.float64) - b.astype(np.float64)).max(axis=1)
    out["compared_seconds"] = round(n / rate, 2)
    out["diff_max_lsb"] = round(float(diff.max()) * 32768, 1)
    out["diff_p99_lsb"] = round(float(np.percentile(diff, 99)) * 32768, 2)
    block = 1024
    nb = n // block
    if nb:
        bd = diff[: nb * block].reshape(nb, block).max(axis=1)
        bad = bd > 64.0 / 32768
        out["blocks"] = int(nb)
        out["blocks_mismatched"] = int(bad.sum())
        out["blocks_mismatched_pct"] = round(100.0 * bad.sum() / nb, 2)
        if bad.any():
            out["first_mismatch_s"] = round(float(np.argmax(bad) * block / rate), 2)
    return out


# ---------------------------------------------------------------------------------------------------------------------
STATS_RE = re.compile(r"audio stats (\w+) t=([\d.]+)s \| (.*)")
KV_RE = re.compile(r"([A-Za-z_()>0-9 ]+?)=(\d+)")


def parse_log(path):
    out = {"stats": [], "underruns": [], "marks": [], "t0": None}
    if not path or not Path(path).exists():
        return out
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        ts = None
        m = re.match(r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]", line)
        if m:
            ts = m.group(1)
        if "audio dump: writing to" in line or ("audio stats" in line and out["t0"] is None):
            pass
        sm = STATS_RE.search(line)
        if sm:
            row = {"label": sm.group(1), "t": float(sm.group(2)), "ts": ts}
            for part in sm.group(3).split("|"):
                part = part.strip()
                head, _, rest = part.partition(":")
                for k, v in KV_RE.findall(rest):
                    row[f"{head.strip()}.{k.strip()}"] = int(v)
            out["stats"].append(row)
        um = re.search(r"audio underrun #(\d+) at t=([\d.]+)s", line)
        if um:
            out["underruns"].append(float(um.group(2)))
        mm = re.search(r"dev pad: mark (\S+) at engine frame", line)
        if mm:
            out["marks"].append({"mark": mm.group(1), "ts": ts})
    return out


def summarize_stats(rows, total_file=None):
    """The run's totals: the final line the game wrote at exit (audio_stats_total.txt in the dump folder), else the LAST cumulative
    TOTAL line of the log (one per interval; the game leaves without a chance to print a final one, up to one interval short)."""
    totals = [r for r in rows if r["label"] == "TOTAL"]
    if total_file and Path(total_file).exists():
        text = Path(total_file).read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            sm = STATS_RE.search(line)
            if sm:
                row = {"label": sm.group(1), "t": float(sm.group(2)), "ts": None, "from_file": True}
                for part in sm.group(3).split("|"):
                    head, _, rest = part.strip().partition(":")
                    for k, v in KV_RE.findall(rest):
                        row[f"{head.strip()}.{k.strip()}"] = int(v)
                totals.append(row)
    return totals[-1] if totals else {}


def pump_rates(rows):
    """Per interval line: guest mixer callbacks per second and guest frames played per second (nominal 187.5: 48000 / 256), from the
    differences of the interval lines' times. Returns {"pump_hz_median", "pump_hz_min", "sdl_frames_hz_median", "sdl_frames_hz_min", "intervals"}
    over the intervals in which the guest was running (pump.n > 0)."""
    iv = [r for r in rows if r["label"] == "interval"]
    pump, play = [], []
    for a, b in zip(iv, iv[1:]):
        dt = b["t"] - a["t"]
        if dt <= 0.5 or not b.get("pump.n"):
            continue
        pump.append(b["pump.n"] / dt)
        if b.get("sdl.frames") is not None:
            play.append(b["sdl.frames"] / dt)
    out = {"intervals": len(pump)}
    if pump:
        out["pump_hz_median"] = round(float(np.median(pump)), 1)
        out["pump_hz_min"] = round(float(min(pump)), 1)
    if play:
        out["sdl_frames_hz_median"] = round(float(np.median(play)), 1)
        out["sdl_frames_hz_min"] = round(float(min(play)), 1)
    return out


# ---------------------------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--dump", required=True)
    ap.add_argument("--log")
    ap.add_argument("--out")
    ap.add_argument("--ref", action="store_true", help="also decode each long stereo context's input with FFmpeg and compare")
    ap.add_argument("--tmp", default=None)
    a = ap.parse_args()

    dump = Path(a.dump)
    rep = {"name": a.name, "dump": str(dump)}
    mix = dump / "mix.wav"
    if mix.exists():
        rep["mix"] = analyze_mix(mix)
    ctxs = []
    for p in sorted(dump.glob("xma_ctx*_s*.wav")):
        try:
            ctxs.append(analyze_ctx(p))
        except Exception as e:  # a half-written file
            ctxs.append({"file": p.name, "error": repr(e)})
    rep["contexts"] = len(ctxs)
    longest = sorted((c for c in ctxs if c.get("channels") == 2 and c.get("seconds", 0) >= 20), key=lambda c: -c["seconds"])
    rep["radio_candidates"] = longest[:3]
    rep["context_summaries"] = [c for c in ctxs if c.get("seconds", 0) >= 5][:60]
    if a.ref:
        tmp = Path(a.tmp) if a.tmp else dump / "_ref"
        rep["reference"] = []
        for c in longest[:3]:
            r = ref_compare(dump / c["file"], tmp)
            r["file"] = c["file"]
            rep["reference"].append(r)
    log = parse_log(a.log)
    rep["stats_total"] = summarize_stats(log["stats"])
    rep["underrun_lines"] = len(log["underruns"])
    rep["underrun_times_s"] = log["underruns"][:20]
    rep["marks"] = log["marks"]
    text = json.dumps(rep, indent=1)
    if a.out:
        Path(a.out).write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()

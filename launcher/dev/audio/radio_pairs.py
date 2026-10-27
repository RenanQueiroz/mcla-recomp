r"""The radio (and any other music) as the decoder produced it: the game plays a stereo song as TWO mono XMA contexts (one per channel,
44.1 kHz, started within a few ms of each other, several hundred input packets, refilled in blocks), so "the longest stereo context" never
finds it. This module pairs the mono contexts of a dump folder and measures the pair as the stereo stream it is.

   python -P -E radio_pairs.py <dump dir> [--ref] [--out report.json] [--all]

For the best pair (the longest decode at wall-clock speed) and, with --all, every pair of at least 20 s:
  * level balance 20 log10(L rms / R rms) per 5 s window, its steps, its drift, windows where one channel plays and the other is silent;
  * the cross-correlation of the two channels per 1 s window over +-1200 samples (27 ms): the lag of the peak and the correlation at lag 0.
    Two channels of one song correlate at lag 0; a decoder that skipped or repeated a frame in ONE channel shifts it by a multiple of 128
    samples (a subframe) or 512 (a frame): "windows_off_lag" counts the windows whose best lag is not 0 (while the lag-0 value is clearly lower);
  * clicks per minute of each channel (audio_analyze.find_clicks), the decode/wall ratio from the sidecars (1.0 = consumed in real time);
  * with --ref: each channel against FFmpeg's xma2 decode of the .xma packets that context was given (mismatched 1024-sample blocks).
Numpy only (no GPU, one core, a few seconds per pair).
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_analyze as aa  # noqa: E402


def load_sidecars(dump):
    rows = []
    for p in Path(dump).glob("xma_ctx*_s*.json"):
        try:
            j = json.loads(p.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            continue
        if not j.get("rate") or not j.get("channels"):
            continue
        j["stem"] = p.stem
        j["decoded_s"] = j["pcm_bytes"] / (j["rate"] * j["channels"] * 2)
        j["wall_s"] = (j["last_us"] - j["first_us"]) / 1e6
        rows.append(j)
    return rows


def find_pairs(dump, min_seconds=20.0, max_start_diff_s=0.1):
    """Pairs of mono contexts that start together, with the same rate and (nearly) the same amount decoded, streaming (>= 3 input blocks):
    the shape of a stereo song played as two mono streams. Sorted longest first."""
    rows = [r for r in load_sidecars(dump) if r["channels"] == 1 and r["decoded_s"] >= min_seconds and len(r.get("blocks", [])) >= 3]
    rows.sort(key=lambda r: r["first_us"])
    used, pairs = set(), []
    for i, a in enumerate(rows):
        if a["stem"] in used:
            continue
        best = None
        for b in rows[i + 1:]:
            if (b["first_us"] - a["first_us"]) / 1e6 > max_start_diff_s:
                break
            if b["stem"] in used or b["rate"] != a["rate"] or abs(b["decoded_s"] - a["decoded_s"]) > 1.5:
                continue
            best = b
            break
        if best is not None:
            used.update((a["stem"], best["stem"]))
            pairs.append((a, best))
    pairs.sort(key=lambda p: -p[0]["decoded_s"])
    return pairs


def xcorr_windows(l, r, rate, win_s=1.0, max_lag=1200):
    """Per window: (best lag, correlation at the best lag, correlation at lag 0), only where both channels are above -45 dBFS."""
    win = int(rate * win_s)
    n = len(l) // win
    out = []
    size = 1
    while size < 2 * win:
        size *= 2
    floor = 10 ** (-45 / 20)
    for k in range(n):
        a = l[k * win:(k + 1) * win].astype(np.float64)
        b = r[k * win:(k + 1) * win].astype(np.float64)
        ea, eb = np.sqrt((a ** 2).mean()), np.sqrt((b ** 2).mean())
        if ea < floor or eb < floor:
            out.append(None)
            continue
        fa, fb = np.fft.rfft(a, size), np.fft.rfft(b, size)
        cc = np.fft.irfft(fa * np.conj(fb), size)  # cc[lag] = sum a[t+lag] b[t]
        norm = win * ea * eb
        lags = np.concatenate((np.arange(0, max_lag + 1), np.arange(-max_lag, 0)))
        vals = np.concatenate((cc[:max_lag + 1], cc[size - max_lag:])) / norm
        j = int(np.argmax(np.abs(vals)))
        out.append((int(lags[j]), float(vals[j]), float(cc[0] / norm)))
    return out


def load_ref(p):
    return np.frombuffer(Path(p).read_bytes(), dtype="<i2").astype(np.float32) / 32768.0


def local_lags(ours, ref, rate, base_lag, win_s=1.0, max_lag=2400):
    win = int(rate * win_s)
    size = 1
    while size < 2 * win + 2 * max_lag:
        size *= 2
    out = []
    for k in range(len(ours) // win):
        a = ours[k * win:(k + 1) * win].astype(np.float64)
        if np.sqrt((a ** 2).mean()) < 0.003:
            out.append(None)
            continue
        s = k * win + base_lag
        lo, hi = s - max_lag, s + win + max_lag
        if lo < 0 or hi > len(ref):
            out.append(None)
            continue
        b = ref[lo:hi].astype(np.float64)
        fa = np.fft.rfft(a, size)
        fb = np.fft.rfft(b, size)
        cc = np.fft.irfft(fb * np.conj(fa), size)  # cc[d] = sum_t b[t + d] a[t]; b starts max_lag before the aligned position
        seg = cc[:2 * max_lag + 1]
        best = int(np.argmax(np.abs(seg)))
        offset = best - max_lag
        nrm = np.sqrt((a ** 2).sum() * (b[best:best + win] ** 2).sum()) + 1e-12
        out.append((offset, round(float(seg[best] / nrm), 3)))
    return out




def ref_steps(dump, stem, rate, tmp):
    """ours against FFmpeg's decode of the same input: the whole-run comparison of audio_analyze.ref_compare plus the local offset per
    second (ours[t] ~ ref[t + lag + offset]): a decoder that loses a frame shows as an offset that steps by 512 samples. Returns
    {mismatch_pct, first_mismatch_s, steps: [(second, change)], max_offset, ...}."""
    wav = Path(dump) / (stem + ".wav")
    res = aa.ref_compare(wav, tmp)
    out = {"stem": stem, "mismatch_pct": res.get("blocks_mismatched_pct"), "first_mismatch_s": res.get("first_mismatch_s"),
           "ref_lag": res.get("lag_frames"), "ref_error": res.get("error"), "ours_s": res.get("ours_seconds"), "ref_s": res.get("ref_seconds")}
    pcm = Path(tmp) / (stem + ".ref.pcm")
    if res.get("error") or not pcm.exists():
        return out
    _, _, ours = aa.read_wav(wav)
    ref = load_ref(pcm)
    ll = local_lags(ours[:, 0], ref, rate, res.get("lag_frames", 0))
    steps, prev, offs = [], None, []
    for k, v in enumerate(ll):
        if v is None or v[1] <= 0.5:
            continue
        offs.append(v[0])
        if prev is not None and v[0] != prev:
            steps.append((k, v[0] - prev))
        prev = v[0]
    out["steps"] = steps[:40]
    out["frames_lost_est"] = sum(1 for _, d in steps if d in (512, 1024))  # whole-frame steps forward (ours ahead of the reference)
    out["max_offset"] = max((abs(o) for o in offs), default=0)
    return out


def analyze_pair(dump, a, b, ref=False, tmp=None):
    dump = Path(dump)
    rate, _, xl = aa.read_wav(dump / (a["stem"] + ".wav"))
    _, _, xr = aa.read_wav(dump / (b["stem"] + ".wav"))
    n = min(len(xl), len(xr))
    st = np.concatenate((xl[:n], xr[:n]), axis=1)
    out = {"left": a["stem"], "right": b["stem"], "rate": rate, "seconds": round(n / rate, 2),
           "start_s": round(a["first_us"] / 1e6, 2), "start_diff_ms": round((b["first_us"] - a["first_us"]) / 1e3, 2),
           "decode_vs_wall": [round(a["decoded_s"] / a["wall_s"], 4) if a["wall_s"] > 5 else None,
                              round(b["decoded_s"] / b["wall_s"], 4) if b["wall_s"] > 5 else None],
           "input_blocks": [len(a.get("blocks", [])), len(b.get("blocks", []))], "input_packets": [a.get("in_packets"), b.get("in_packets")]}
    out["balance"] = aa.balance_stats(st, rate)
    series = aa.balance_series(st, rate)
    out["balance_series_5s"] = series
    out["balance_trend"] = aa.balance_trend(series)
    cl, cr = aa.find_clicks(xl[:n], rate), aa.find_clicks(xr[:n], rate)
    out["clicks_left"], out["clicks_right"] = len(cl), len(cr)
    out["clicks_per_min"] = round((len(cl) + len(cr)) / 2 / (n / rate) * 60, 2) if n else 0
    xc = xcorr_windows(xl[:n, 0], xr[:n, 0], rate)
    ok = [v for v in xc if v is not None]
    if ok:
        c0 = np.array([v[2] for v in ok])
        off = [v for v in ok if v[0] != 0 and abs(v[1]) > 0.3 and abs(v[1]) > abs(v[2]) + 0.15]
        out["xcorr"] = {"windows": len(ok), "lag0_corr_median": round(float(np.median(c0)), 3), "lag0_corr_p5": round(float(np.percentile(c0, 5)), 3),
                        "windows_off_lag": len(off), "off_lags": sorted({v[0] for v in off})[:12],
                        "lag_series": [None if v is None else v[0] for v in xc][:120]}
    if ref:
        tmp = Path(tmp or (dump / "_ref"))
        out["reference"] = [ref_steps(dump, c["stem"], rate, tmp) for c in (a, b)]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--ref", action="store_true")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--tmp")
    a = ap.parse_args()
    pairs = find_pairs(a.dump)
    res = {"dump": a.dump, "pairs_found": len(pairs), "pairs": []}
    # the radio: the pair that decoded at wall-clock speed and is longest; others only with --all
    for k, (l, r) in enumerate(pairs[: (6 if a.all else 1)]):
        res["pairs"].append(analyze_pair(a.dump, l, r, ref=a.ref and k == 0, tmp=a.tmp))
    text = json.dumps(res, indent=1)
    if a.out:
        Path(a.out).write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()

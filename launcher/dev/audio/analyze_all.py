r"""Analysis of the audio batches: every run's dumps and game.log (audio_analyze.py), a table per variant, the decision rules, and
the two listening clips (old and new) made from the first batch.

   python -P -E analyze_all.py --runs <audio\runs folder> --dumps C:\mcv\audio\dev\dumps --out <results folder> [--batches A,B,C,D] [--ref]
                               [--clips <folder>]

Writes <out>\<batch>-<tag>.json per run, <out>\SUMMARY.md (the table and the verdict) and, with --clips, the clips.
"""
import argparse
import json
import re
import statistics
import sys
from datetime import datetime
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_analyze as aa  # noqa: E402
import radio_pairs as rp  # noqa: E402


def load_results(runs_dir, batch):
    p = Path(runs_dir) / batch / "results.jsonl"
    rows = []
    if p.exists():
        for line in p.read_text(encoding="utf-8").splitlines():
            try:
                rows.append(json.loads(line))
            except ValueError:
                pass
    return rows


def ts_seconds(ts):
    return datetime.strptime(ts, "%Y-%m-%d %H:%M:%S.%f").timestamp()


def log_times(log_path):
    """{"dump_t0": wall time of the audio clock's zero, "marks": {name: wall time}} from game.log."""
    out = {"dump_t0": None, "marks": {}}
    if not Path(log_path).exists():
        return out
    for line in Path(log_path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]", line)
        if not m:
            continue
        if "audio dump: writing to" in line and out["dump_t0"] is None:
            out["dump_t0"] = ts_seconds(m.group(1))
        mm = re.search(r"dev pad: mark (\S+) at engine frame", line)
        if mm and mm.group(1) not in out["marks"]:
            out["marks"][mm.group(1)] = ts_seconds(m.group(1))
    return out


def mark_audio_time(times, mark):
    if times["dump_t0"] is None or mark not in times["marks"]:
        return None
    return times["marks"][mark] - times["dump_t0"]


def stereo_fold(x):
    """6 channels (fl fr fc lf bl br) to 2 with the SDK's default fold; 2 channels as they are."""
    if x.shape[1] == 2:
        return x
    c = 0.70710678
    scale = 0.58578644
    left = (x[:, 0] + c * x[:, 2] + c * x[:, 4]) * scale
    right = (x[:, 1] + c * x[:, 2] + c * x[:, 5]) * scale
    return np.stack([left, right], axis=1)


def write_clip(mix_path, t0, seconds, dest):
    rate, ch, x = aa.read_wav(mix_path)
    a = max(0, int(t0 * rate))
    b = min(len(x), a + int(seconds * rate))
    clip = np.clip(stereo_fold(x[a:b]), -1.0, 1.0)
    # short fades so the clip does not click at its ends
    n = min(2400, len(clip) // 4)
    if n:
        ramp = np.linspace(0, 1, n)[:, None]
        clip[:n] *= ramp
        clip[-n:] *= ramp[::-1]
    pcm = (clip * 32767.0).astype("<i2").tobytes()
    hdr = (b"RIFF" + (36 + len(pcm)).to_bytes(4, "little") + b"WAVEfmt " + (16).to_bytes(4, "little") + (1).to_bytes(2, "little") +
           (2).to_bytes(2, "little") + rate.to_bytes(4, "little") + (rate * 4).to_bytes(4, "little") + (4).to_bytes(2, "little") +
           (16).to_bytes(2, "little") + b"data" + len(pcm).to_bytes(4, "little"))
    Path(dest).write_bytes(hdr + pcm)
    return {"file": str(dest), "from_s": round(a / rate, 2), "seconds": round(len(clip) / rate, 2),
            "gaps": len(aa.find_gaps(clip)), "clicks": len(aa.find_clicks(clip, rate)),
            "peak": round(float(np.abs(clip).max()), 3)}


NOMINAL_HZ = 48000 / 256  # guest audio frames (256 samples at 48 kHz) per second: 187.5


def frame_rate_section(table, out):
    """The frame-rate test: the shipped behaviour (variant old = every new switch off) at --mcla_fps=30, 60 and uncapped on the radio
    drive. Batch D has the three caps (30 and 60 twice); the uncapped runs of batch A (old1, old2: the same route, the same switches
    off) are counted with batch D's uncapped runs. Returns the SUMMARY lines and writes D-radio-balance.csv (the balance over time)."""
    groups = {"30": [], "60": [], "uncapped": []}
    for r in table:
        if r.get("error") or r.get("note") or r["variant"] != "old":
            continue
        if r["batch"] == "D":
            cap = str(r.get("fps_cap"))
            groups["uncapped" if cap in ("0", "None") else cap].append(r)
        elif r["batch"] == "A" and r["tag"] != "warm":
            groups["uncapped"].append(r)
    lines = ["", "## Frame rate: the shipped behaviour (every new switch off) at 30, 60 and uncapped FPS", ""]
    if not groups["30"] or not (groups["60"] or groups["uncapped"]):
        lines.append("- batch D has not run (or too little of it): no frame-rate verdict")
        return lines
    cols = ["batch", "tag", "fps_cap", "drive_fps", "radio_s", "radio_bal_std", "radio_bal_step_p95", "radio_bal_slope", "radio_bal_drift",
            "radio_dropouts", "radio_clicks_per_min", "gaps", "clicks_per_min", "underruns", "pump_hz", "pump_hz_min", "play_hz",
            "radio_decode_ratio", "pump_late", "pump_over8", "xma_us_per_pump", "radio_file"]
    lines += ["| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    rows = [r for g in ("30", "60", "uncapped") for r in groups[g]]
    for r in rows:
        lines.append("| " + " | ".join("" if r.get(c) is None else str(r.get(c)) for c in cols) + " |")
    lines += ["", "Columns: drive_fps = the game's mean FPS during the drive; radio_* = the longest stereo XMA stream (the radio) as the decoder "
              "produced it, balance = 20 log10(L rms / R rms) per 5 s window; radio_bal_slope = its least-squares slope in dB/min, radio_bal_drift = "
              "median of the last third minus the first third; pump_hz / play_hz = guest mixer callbacks / guest frames played per second "
              f"(nominal {NOMINAL_HZ}; _min = the worst interval); radio_decode_ratio = audio seconds the decoder produced for the radio stream / wall "
              "seconds it was active (1.0 = consumed in real time; another value = the game's audio clock runs at another speed than the wall "
              "clock). The uncapped rows of batch A are the same route with the same switches off.", ""]
    # the balance over time
    lines += ["Radio balance in dB (L - R) per 5 s window from the start of the stream (`.` = a channel silent):", "", "```"]
    for r in rows:
        ser = r.get("radio_series") or []
        lines.append(f"{r['batch']}-{r['tag']:<8} cap {str(r.get('fps_cap')):>3}: "
                     + " ".join("." if v is None else f"{v:+.1f}" for v in ser[:36]))
    lines += ["```", ""]
    width = max((len(r.get("radio_series") or []) for r in rows), default=0)
    csv = ["t_s," + ",".join(f"{r['batch']}-{r['tag']}" for r in rows)]
    for i in range(width):
        csv.append(str(i * 5) + "," + ",".join(
            "" if i >= len(r.get("radio_series") or []) or r["radio_series"][i] is None else str(r["radio_series"][i]) for r in rows))
    (out / "D-radio-balance.csv").write_text("\n".join(csv) + "\n", encoding="utf-8")

    def med(g, key):
        vals = [r.get(key) for r in groups[g] if isinstance(r.get(key), (int, float))]
        return statistics.median(vals) if vals else None

    def fmt(v):
        return "n/a" if v is None else (f"{v:.2f}" if isinstance(v, float) else str(v))

    lines.append("Medians per frame-rate cap (runs: " + ", ".join(f"{g}: {len(groups[g])}" for g in ("30", "60", "uncapped")) + "):")
    lines.append("")
    # (key, label, the absolute change that matters)
    metrics = [("radio_bal_step_p95", "radio balance step p95 (dB)", 1.0), ("radio_bal_std", "radio balance std (dB)", 1.5),
               ("radio_bal_slope", "radio balance slope (dB/min, absolute)", 0.5), ("radio_dropouts", "radio one-channel dropout windows", 1),
               ("radio_clicks_per_min", "radio clicks per minute", 2), ("gaps", "gaps in the mix", 2),
               ("clicks_per_min", "clicks per minute in the mix", 2), ("underruns", "SDL underruns", 3),
               ("pump_late", "late guest mixer callbacks", 50)]
    flags = []
    for key, label, thr in metrics:
        vals = {g: med(g, key) for g in ("30", "60", "uncapped")}
        if key == "radio_bal_slope":
            vals = {g: (abs(v) if v is not None else None) for g, v in vals.items()}
        base = vals["30"]
        worse = []
        for g in ("60", "uncapped"):
            v = vals[g]
            if v is not None and base is not None and v - base > thr and (base == 0 or v >= 1.5 * base):
                worse.append(g)
        flags += [(label, g) for g in worse]
        lines.append(f"- {label}: 30 = {fmt(vals['30'])}, 60 = {fmt(vals['60'])}, uncapped = {fmt(vals['uncapped'])}"
                     + (f"  <- WORSE ABOVE 30 ({', '.join(worse)})" if worse else ""))
    ratios = {g: med(g, "radio_decode_ratio") for g in ("30", "60", "uncapped")}
    hz = {g: med(g, "pump_hz") for g in ("30", "60", "uncapped")}
    for label, vals, ref, thr in (("radio decode/wall ratio (1.0 = real time)", ratios, 1.0, 0.02),
                                  ("guest mixer callbacks per second", hz, NOMINAL_HZ, NOMINAL_HZ * 0.02)):
        worse = [g for g in ("60", "uncapped") if vals[g] is not None and vals["30"] is not None
                 and abs(vals[g] - ref) - abs(vals["30"] - ref) > thr]
        flags += [(label, g) for g in worse]
        lines.append(f"- {label}: 30 = {fmt(vals['30'])}, 60 = {fmt(vals['60'])}, uncapped = {fmt(vals['uncapped'])}"
                     + (f"  <- FURTHER FROM NOMINAL ABOVE 30 ({', '.join(worse)})" if worse else ""))
    anything = any((med(g, k) or 0) > 0 for g in ("30", "60", "uncapped") for k in ("radio_dropouts", "gaps", "underruns")) or any(
        (med(g, "radio_bal_step_p95") or 0) > 1.0 for g in ("30", "60", "uncapped"))
    lines.append("")
    if flags:
        lines.append("**Frame-rate dependent on this PC: YES** (" + "; ".join(f"{label} at {g}" for label, g in flags[:6])
                     + "). The effect shows only above 30 FPS: the cause is likely game-side timing (audio code ticked per frame) rather than the "
                     "decoder; compare with batch A (old vs new) and check whether a cap (--mcla_fps=30 or 60) removes it for the player.")
    elif anything:
        lines.append("**Frame-rate dependent on this PC: NO.** The effects that exist (the non-zero values above) are the same at 30, 60 and uncapped.")
    else:
        lines.append("**Frame-rate dependent on this PC: NOT SHOWN.** Nothing (dropouts, gaps, underruns, balance steps) shows at any rate, so this "
                     "run cannot tell (the player's reports are from a slower CPU): the 'old' rows of batches A and C carry the same caveat.")
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", required=True)
    ap.add_argument("--dumps", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--batches", default="A,B,C,D")
    ap.add_argument("--ref", action="store_true")
    ap.add_argument("--clips")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    table = []
    for batch in a.batches.split(","):
        for row in load_results(a.runs, batch):
            cfg = row.get("cfg", {})
            tag = cfg.get("tag", row.get("tag", "?"))
            if tag == "warm" and False:
                continue
            name = f"{batch}-{tag}"
            dump = Path(a.dumps) / name
            runfolder = Path(a.runs) / batch / tag
            log = runfolder / "game.log"
            segs = row.get("segments") or {}
            rec = {"batch": batch, "tag": tag, "variant": cfg.get("variant"), "aff": cfg.get("aff"), "fps_cap": cfg.get("fps"),
                   "exit": row.get("exit_code"),
                   "drive_fps": round((segs.get("drive") or {}).get("mean_fps", 0), 1) or None,
                   "error": row.get("error"), "bad": row.get("bad"), "seconds": row.get("seconds")}
            if not dump.exists():
                rec["note"] = "no dump folder"
                table.append(rec)
                continue
            rep_path = out / f"{name}.json"
            cmd_args = argparse.Namespace()
            rep = {"name": name}
            mix = dump / "mix.wav"
            if mix.exists():
                rep["mix"] = aa.analyze_mix(mix)
            ctxs = []
            for p in sorted(dump.glob("xma_ctx*_s*.wav")):
                try:
                    ctxs.append(aa.analyze_ctx(p))
                except Exception as e:
                    ctxs.append({"file": p.name, "error": repr(e)})
            # The radio is TWO mono contexts (one per channel, started together, streaming): pair them (radio_pairs.py). A stereo context of
            # 20 s or more, if there is one, is kept as a fallback.
            longest = sorted((c for c in ctxs if c.get("channels") == 2 and c.get("seconds", 0) >= 20), key=lambda c: -c["seconds"])
            rep["radio_candidates"] = longest[:3]
            rep["contexts"] = len(ctxs)
            rep["radio_pairs"] = []
            try:
                for k, (pl, pr) in enumerate(rp.find_pairs(dump)[:3]):
                    rep["radio_pairs"].append(rp.analyze_pair(dump, pl, pr, ref=True, tmp=out / "_ref" / name))
            except Exception as e:  # keep the run's other numbers
                rep["radio_pairs_error"] = repr(e)
            parsed = aa.parse_log(log)
            rep["stats_total"] = aa.summarize_stats(parsed["stats"], dump / "audio_stats_total.txt")
            rep["rates"] = aa.pump_rates(parsed["stats"])
            rep["underrun_lines"] = len(parsed["underruns"])
            rep["times"] = log_times(log)
            rep_path.write_text(json.dumps(rep, indent=1), encoding="utf-8")
            st = rep["stats_total"]
            mixr = rep.get("mix", {})
            pair = (rep.get("radio_pairs") or [None])[0]
            radio = {}
            if pair:
                radio = {"seconds": pair["seconds"], "balance": pair["balance"], "balance_trend": pair["balance_trend"],
                         "clicks_per_min": pair["clicks_per_min"], "decode_vs_wall": (pair["decode_vs_wall"] or [None])[0],
                         "balance_series_5s": pair["balance_series_5s"], "file": pair["left"] + "+" + pair["right"]}
            elif longest:
                radio = longest[0]
            refs = pair.get("reference", []) if pair else []
            mism = [r.get("mismatch_pct") for r in refs if r.get("mismatch_pct") is not None]
            xc = (pair or {}).get("xcorr", {})
            ref0 = {"lag_frames": (refs[0].get("ref_lag") if refs else None), "blocks_mismatched_pct": (max(mism) if mism else None),
                    "diff_max_lsb": None, "error": (refs[0].get("ref_error") if refs else None)}
            rec.update({
                "mix_s": mixr.get("seconds"), "gaps": mixr.get("gaps"), "gaps_per_min": mixr.get("gaps_per_min"),
                "gap_ms_total": mixr.get("gap_ms_total"), "clicks_per_min": mixr.get("clicks_per_min"),
                "mix_bal_step_p95": (mixr.get("balance") or {}).get("balance_step_db_p95"),
                "radio_s": radio.get("seconds"), "radio_bal_std": (radio.get("balance") or {}).get("balance_db_std"),
                "radio_bal_step_p95": (radio.get("balance") or {}).get("balance_step_db_p95"),
                "radio_dropouts": (radio.get("balance") or {}).get("dropout_windows"),
                "radio_clicks_per_min": radio.get("clicks_per_min"),
                "underruns": st.get("sdl.underruns"), "queue_min": st.get("sdl.queue_min"),
                "wrote_rd": st.get("game wrote during a decode.read_offset"), "wrote_valid": st.get("game wrote during a decode.valid"),
                "wrote_other": st.get("game wrote during a decode.other"),
                "work_inline": st.get("xma.work inline"), "work_worker": st.get("xma.worker"), "work_slow": st.get("xma.slow(>2ms)"),
                "xma_errors": st.get("xma.errors"), "recheck_abort": st.get("xma.recheck_abort"), "stalled": st.get("xma.stalled"),
                "cas_retry": st.get("xma.cas_retry"), "pump_late": st.get("pump.late(>8ms)"), "pump_gap_max_us": st.get("pump.gap_max_us"),
                "pump_max_us": st.get("pump.max_us"), "pump_avg_us": st.get("pump.avg_us"), "pump_over4": st.get("pump.over4ms"),
                "pump_over8": st.get("pump.over8ms"),
                # what the inline XMA decodes cost per mixer callback (they run inside it when the guest kicks a context)
                "xma_us_per_pump": (round((st.get("xma.work inline") or 0) * (st.get("xma.avg_us") or 0) / st["pump.n"])
                                    if st.get("pump.n") else None),
                "radio_bal_slope": (radio.get("balance_trend") or {}).get("slope_db_per_min"),
                "radio_bal_drift": (radio.get("balance_trend") or {}).get("drift_db"),
                "radio_decode_ratio": radio.get("decode_vs_wall"),
                "pump_hz": rep["rates"].get("pump_hz_median"), "pump_hz_min": rep["rates"].get("pump_hz_min"),
                "play_hz": rep["rates"].get("sdl_frames_hz_median"), "play_hz_min": rep["rates"].get("sdl_frames_hz_min"),
                "radio_series": radio.get("balance_series_5s"), "radio_file": radio.get("file"),
                "radio_lr_corr": xc.get("lag0_corr_median"), "radio_lr_off_lag": xc.get("windows_off_lag"),
                "radio_first_mismatch_s": (min((r.get("first_mismatch_s") for r in refs if r.get("first_mismatch_s") is not None), default=None)
                                           if mism and max(mism) > 1.0 else None),
                "radio_frames_lost": sum(r.get("frames_lost_est") or 0 for r in refs) if refs else None,
                "ref_lag": ref0.get("lag_frames"), "ref_mismatch_pct": ref0.get("blocks_mismatched_pct"),
                "ref_diff_max_lsb": ref0.get("diff_max_lsb"), "ref_error": ref0.get("error"),
            })
            table.append(rec)

    # ---- SUMMARY.md
    cols = ["batch", "tag", "variant", "aff", "fps_cap", "drive_fps", "exit", "mix_s", "underruns", "gaps", "gap_ms_total", "clicks_per_min", "mix_bal_step_p95",
            "radio_s", "radio_bal_std", "radio_bal_step_p95", "radio_dropouts", "radio_clicks_per_min", "radio_lr_corr", "radio_lr_off_lag",
            "radio_frames_lost", "wrote_rd", "wrote_valid",
            "wrote_other", "work_inline", "work_worker", "work_slow", "xma_errors", "recheck_abort", "stalled", "cas_retry", "pump_late",
            "pump_gap_max_us", "pump_avg_us", "pump_over4", "pump_over8", "xma_us_per_pump", "queue_min", "ref_lag", "ref_mismatch_pct", "ref_diff_max_lsb", "error"]
    lines = ["# Audio checks: results", "", "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in table:
        lines.append("| " + " | ".join("" if r.get(c) is None else str(r.get(c)) for c in cols) + " |")
    lines += ["", "Columns: underruns = SDL callbacks that had to play silence (sdl.underruns); gaps = runs of digital silence in mix.wav; "
              "wrote_* = what the game wrote into an XMA context during a Work() call (old: overwritten by the stale snapshot, new: kept); "
              "work_worker = Work() calls done by the XMA worker thread; pump_late = guest mixer callbacks more than 8 ms after the previous "
              "one; ref_* = the radio context against FFmpeg's xma2 decode of its input (batch A only).", ""]

    def med(vals):
        vals = [v for v in vals if isinstance(v, (int, float))]
        return statistics.median(vals) if vals else None

    verdict = ["## Decision rules", ""]
    A = [r for r in table if r["batch"] == "A" and r["tag"] != "warm" and not r.get("error")]
    olds = [r for r in A if r["variant"] == "old"]
    news = [r for r in A if r["variant"] == "new"]
    if olds and news:
        def total(rs, k):
            return sum(v for v in (r.get(k) for r in rs) if isinstance(v, (int, float)))
        v_ok = True
        verdict.append(f"- runs: {len(olds)} old, {len(news)} new (batch A, 12 logical processors)")
        verdict.append(f"- the SDK's own overwrite is real in `old`: read offsets the game wrote during a decode = {total(olds, 'wrote_rd')} (new: kept, "
                       f"{total(news, 'wrote_rd')}); valid bits {total(olds, 'wrote_valid')} / {total(news, 'wrote_valid')}; other {total(olds, 'wrote_other')} / {total(news, 'wrote_other')}")
        verdict.append(f"- Work() calls done by the XMA worker thread (race: the worker deciding a kicked context): old {total(olds, 'work_worker')}, new {total(news, 'work_worker')}")
        e_old, e_new = total(olds, "xma_errors"), total(news, "xma_errors")
        verdict.append(f"- XMA decode errors: old {e_old}, new {e_new}" + ("" if e_new <= e_old else "  <- NEW IS WORSE"))
        v_ok &= e_new <= e_old
        s_new = total(news, "stalled")
        verdict.append(f"- decode loops ended for lack of progress in new: {s_new}")
        u_old, u_new = total(olds, "underruns"), total(news, "underruns")
        verdict.append(f"- SDL underruns: old {u_old}, new {u_new}" + ("" if u_new <= u_old else "  <- NEW IS WORSE"))
        v_ok &= u_new <= u_old
        g_old, g_new = total(olds, "gaps"), total(news, "gaps")
        verdict.append(f"- gaps in the mix: old {g_old}, new {g_new}" + ("" if g_new <= g_old else "  <- NEW IS WORSE"))
        v_ok &= g_new <= g_old
        c_old, c_new = med([r.get("clicks_per_min") for r in olds]), med([r.get("clicks_per_min") for r in news])
        verdict.append(f"- clicks per minute in the mix (median): old {c_old}, new {c_new}")
        if c_old is not None and c_new is not None:
            v_ok &= c_new <= c_old * 1.25 + 0.5
        m_old, m_new = med([r.get("ref_mismatch_pct") for r in olds]), med([r.get("ref_mismatch_pct") for r in news])
        verdict.append(f"- the radio context against the FFmpeg reference, blocks over 64 LSB off (median %): old {m_old}, new {m_new}")
        if m_old is not None and m_new is not None:
            v_ok &= m_new <= m_old + 0.5
        d_old, d_new = med([r.get("radio_dropouts") for r in olds]), med([r.get("radio_dropouts") for r in news])
        verdict.append(f"- one-channel dropout windows in the radio context: old {d_old}, new {d_new}")
        if d_old is not None and d_new is not None:
            v_ok &= d_new <= d_old
        verdict.append("")
        verdict.append("**No regression in `new` on these measures: " + ("YES" if v_ok else "NO") + ".** A YES means the switches may be turned on "
                       "by default (flip the defaults in the patch, see NOTES.md); it is not evidence that the player's reports are fixed "
                       "unless `old` shows the effect (underruns, late pumps, overwritten read offsets, dropouts) at all.")
    else:
        verdict.append("- batch A is incomplete: no verdict")
    verdict += frame_rate_section(table, out)
    (out / "SUMMARY.md").write_text("\n".join(lines + verdict) + "\n", encoding="utf-8")
    print("\n".join(lines + verdict))

    # ---- clips
    if a.clips:
        clips = Path(a.clips)
        clips.mkdir(parents=True, exist_ok=True)
        made = []
        for variant, tags in (("old", ("old1", "old2")), ("new", ("new1", "new2"))):
            for tag in tags:
                name = f"A-{tag}"
                mix = Path(a.dumps) / name / "mix.wav"
                log = Path(a.runs) / "A" / tag / "game.log"
                if not mix.exists():
                    continue
                times = log_times(log)
                for label, mark, offset, length in (("radio", "drive", -12.0, 30.0), ("title", "title", 6.0, 30.0)):
                    t = mark_audio_time(times, mark)
                    if t is None:
                        continue
                    made.append({"variant": variant, "run": name, "clip": label,
                                 **write_clip(mix, max(0.0, t + offset), length, clips / f"{variant}-{label}-{tag}.wav")})
                break  # one run per variant: the first that has a dump
        (clips / "CLIPS.json").write_text(json.dumps(made, indent=1), encoding="utf-8")
        print(json.dumps(made, indent=1))


if __name__ == "__main__":
    main()

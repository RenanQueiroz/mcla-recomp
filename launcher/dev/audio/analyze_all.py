r"""Analysis of the audio batches: every run's dumps and game.log (audio_analyze.py), a table per variant, the decision rules, and
the two listening clips (old and new) made from the first batch.

   python -P -E analyze_all.py --runs <audio\runs folder> --dumps C:\mcv\audio\dev\dumps --out <results folder> [--batches A,B,C] [--ref]
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", required=True)
    ap.add_argument("--dumps", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--batches", default="A,B,C")
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
            rec = {"batch": batch, "tag": tag, "variant": cfg.get("variant"), "aff": cfg.get("aff"),
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
            longest = sorted((c for c in ctxs if c.get("channels") == 2 and c.get("seconds", 0) >= 20), key=lambda c: -c["seconds"])
            rep["radio_candidates"] = longest[:3]
            rep["contexts"] = len(ctxs)
            if a.ref and batch == "A" and tag != "warm":
                rep["reference"] = []
                for c in longest[:2]:
                    r = aa.ref_compare(dump / c["file"], out / "_ref" / name)
                    r["file"] = c["file"]
                    rep["reference"].append(r)
            parsed = aa.parse_log(log)
            rep["stats_total"] = aa.summarize_stats(parsed["stats"])
            rep["underrun_lines"] = len(parsed["underruns"])
            rep["times"] = log_times(log)
            rep_path.write_text(json.dumps(rep, indent=1), encoding="utf-8")
            st = rep["stats_total"]
            mixr = rep.get("mix", {})
            radio = longest[0] if longest else {}
            ref0 = (rep.get("reference") or [{}])[0]
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
                "ref_lag": ref0.get("lag_frames"), "ref_mismatch_pct": ref0.get("blocks_mismatched_pct"),
                "ref_diff_max_lsb": ref0.get("diff_max_lsb"), "ref_error": ref0.get("error"),
            })
            table.append(rec)

    # ---- SUMMARY.md
    cols = ["batch", "tag", "variant", "aff", "mix_s", "underruns", "gaps", "gap_ms_total", "clicks_per_min", "mix_bal_step_p95",
            "radio_s", "radio_bal_std", "radio_bal_step_p95", "radio_dropouts", "radio_clicks_per_min", "wrote_rd", "wrote_valid",
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

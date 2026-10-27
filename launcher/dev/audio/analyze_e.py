r"""Analysis of the second audio round (batches E0, E1): underruns that matter, how late the audio worker answers, the radio against the
reference, the pump-sleep cap, and which switch (if any) may become a default.

   python -P -E analyze_e.py --runs <audio\runs> --dumps C:\mcv\audio\dev\dumps --out <results dir> [--clips <clips dir>]

Writes <out>\E-SUMMARY.md, <out>\E-<tag>.json per run and (with --clips) listening clips of the runs e4-ship (the SDK's decoder) and e4-best.
Light on the CPU (about 10 s per run; run it under the lock anyway).
"""
import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_analyze as aa  # noqa: E402
import radio_pairs as rp  # noqa: E402
import analyze_all as aall  # noqa: E402

TICK_RE = re.compile(r"came round (\d+) times, ([\d.]+) us apart on average \(([\d.]+) ms at most\)")


def load_rows(runs, batch):
    p = Path(runs) / batch / "results.jsonl"
    rows = []
    if p.exists():
        for line in p.read_text(encoding="utf-8").splitlines():
            try:
                rows.append(json.loads(line))
            except ValueError:
                pass
    return rows


def last_tick(log):
    found = None
    if Path(log).exists():
        for line in Path(log).read_text(encoding="utf-8", errors="replace").splitlines():
            m = TICK_RE.search(line)
            if m:
                found = (int(m.group(1)), float(m.group(2)), float(m.group(3)))
    return found


def analyze_run(batch, row, runs, dumps, out):
    cfg = row.get("cfg", {})
    tag = cfg.get("tag", row.get("tag"))
    name = f"{batch}-{tag}"
    dump = Path(dumps) / name
    log = Path(runs) / batch / tag / "game.log"
    segs = row.get("segments") or {}
    rec = {"name": name, "tag": tag, "aff": cfg.get("aff"), "exit": row.get("exit_code"), "seconds": row.get("seconds"),
           "error": row.get("error"), "bad": row.get("bad"), "drive_fps": round((segs.get("drive") or {}).get("mean_fps", 0), 1) or None,
           "drive_p99_ms": (segs.get("drive") or {}).get("p99_ms"), "extra": [e for e in cfg.get("extra", []) if not e.startswith("--audio_dump") and e not in (
               "--audio_mute=true", "--audio_stats=true", "--audio_stats_interval=5", "--mcla_poll_stats=true")]}
    parsed = aa.parse_log(log)
    st = aa.summarize_stats(parsed["stats"], dump / "audio_stats_total.txt")
    rec["stats"] = st
    iv = [r for r in parsed["stats"] if r["label"] == "interval" and r["t"] > 10.0]
    rec["queue_min_after10s"] = min((r.get("sdl.queue_min", 0) for r in iv), default=None)
    rec["underruns_after10s"] = sum(r.get("sdl.underruns", 0) for r in iv)
    rec["underruns_steady_by_interval"] = [(r["t"], r.get("sdl.underruns_steady", 0)) for r in iv if r.get("sdl.underruns_steady", 0)][:12]
    rec["pump_gap_max_ms_after10s"] = round(max((r.get("pump.gap_max_us", 0) for r in iv), default=0) / 1000.0, 1)
    rec["rates"] = aa.pump_rates(parsed["stats"])
    tick = last_tick(log)
    rec["pump_thread_hz"] = round(1e6 / tick[1], 1) if tick and tick[1] else None
    mix = dump / "mix.wav"
    if mix.exists():
        _, _, x = aa.read_wav(mix)
        gaps = aa.find_gaps(x)
        rec["mix_gaps"] = len(gaps)
        rec["mix_gap_ms_total"] = round(sum(g[1] for g in gaps) / 48.0, 1)
        rec["mix_seconds"] = round(len(x) / 48000.0, 1)
        del x
    try:
        pairs = rp.find_pairs(dump)
        if pairs:
            res = rp.analyze_pair(dump, pairs[0][0], pairs[0][1], ref=True, tmp=out / "_ref" / name)
            refs = res.get("reference", [])
            mism = [r.get("mismatch_pct") for r in refs if r.get("mismatch_pct") is not None]
            rec["radio"] = {"frames_lost": sum(r.get("frames_lost_est") or 0 for r in refs), "mismatch_pct": max(mism) if mism else None,
                            "lr_corr": res.get("xcorr", {}).get("lag0_corr_median"), "off_lag_s": res.get("xcorr", {}).get("windows_off_lag"),
                            "bal_std": res.get("balance", {}).get("balance_db_std"), "seconds": res.get("seconds")}
            rec["_pair"] = {"left": pairs[0][0]["stem"], "right": pairs[0][1]["stem"], "start_s": pairs[0][0]["first_us"] / 1e6}
    except Exception as e:
        rec["radio_error"] = repr(e)
    (out / f"{name}.json").write_text(json.dumps(rec, indent=1, default=str), encoding="utf-8")
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", required=True)
    ap.add_argument("--dumps", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--clips")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    recs = []
    for batch in ("E0", "E1"):
        for row in load_rows(a.runs, batch):
            if not (Path(a.dumps) / f"{batch}-{row.get('cfg', {}).get('tag')}").exists():
                continue
            try:
                recs.append(analyze_run(batch, row, a.runs, a.dumps, out))
            except Exception as e:
                recs.append({"name": f"{batch}-{row.get('cfg', {}).get('tag')}", "tag": row.get("cfg", {}).get("tag"), "error": repr(e)})
    by = {r["tag"]: r for r in recs}

    def g(r, k, default=""):
        v = r.get(k)
        return default if v is None else v

    def s(r, key):
        v = r.get("stats", {}).get(key)
        return "" if v is None else v

    cols = ["run", "aff", "drive_fps", "pump_thread_hz", "exit", "underruns (all)", "steady underruns", "silence_max (frames)", "queue_min after 10 s",
            "queue_grown", "wake <1 ms", "<2", "<4", "<8", "<16", ">=16 ms", "wake max us", "pump gap max ms (after 10 s)", "pump avg us",
            "over8ms", "xma worker", "wrote rd", "mix gaps", "radio frames lost", "radio mismatch %", "radio L/R corr", "error"]
    lines = ["# Audio, round 2: the output queue, the mixer thread and the pump-sleep cap", "", "| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for r in recs:
        rad = r.get("radio", {})
        lines.append("| " + " | ".join(str(x) for x in [
            r["name"], g(r, "aff"), g(r, "drive_fps"), g(r, "pump_thread_hz"), g(r, "exit"), s(r, "sdl.underruns"), s(r, "sdl.underruns_steady"),
            s(r, "sdl.silence_max"), g(r, "queue_min_after10s"), s(r, "sdl.queue_grown"), s(r, "wake.lt1ms"), s(r, "wake.lt2ms"), s(r, "wake.lt4ms"),
            s(r, "wake.lt8ms"), s(r, "wake.lt16ms"), s(r, "wake.ge16ms"), s(r, "wake.max_us"), g(r, "pump_gap_max_ms_after10s"), s(r, "pump.avg_us"),
            s(r, "pump.over8ms"), s(r, "xma.worker"), s(r, "game wrote during a decode.read_offset"), g(r, "mix_gaps"), g(rad, "frames_lost"),
            g(rad, "mismatch_pct"), g(rad, "lr_corr"), (r.get("error") or "")[:80]]) + " |")
    lines += ["", "Columns: steady underruns = silent frames played after the game's first second of audio (the ~50 at the start of every run are the device "
              "running before the game's mixer and are in 'underruns (all)' only); silence_max = the longest run of them; queue_min = fewest frames queued "
              "at the start of an output callback, after the first 10 s (of audio_maxqframes 8, or more); wake = time from the output freeing a queue slot to "
              "the start of the mixer callback (the audio worker's response time); pump_thread_hz = rounds a second of the game's thread with the 7 ms "
              "sleep (mcla_pump_sleep_max_ms); radio = the radio pair against FFmpeg's decode (0 frames lost and ~0 % = identical).", ""]

    verdict = ["## Verdict", ""]
    base4, base16 = by.get("e4-base"), by.get("e16-base")
    ship = by.get("e4-ship")

    def steady(r):
        v = r.get("stats", {}).get("sdl.underruns_steady")
        return v if isinstance(v, (int, float)) else None

    def late(r):
        st = r.get("stats", {})
        return (st.get("wake.lt16ms", 0) or 0) + (st.get("wake.ge16ms", 0) or 0)

    for label, b in (("16 logical processors", base16), ("4 logical processors", base4)):
        if b is None:
            verdict.append(f"- {label}: no base run")
            continue
        verdict.append(f"- {label}, shipped defaults: {steady(b)} steady underruns, longest silence {b.get('stats', {}).get('sdl.silence_max')} frames, "
                       f"{late(b)} wake latencies of 8 ms or more, worst {b.get('stats', {}).get('wake.max_us')} us, queue never below {b.get('queue_min_after10s')}, "
                       f"{b.get('drive_fps')} FPS in the drive")
    if ship and base4:
        verdict.append(f"- the SDK's decoder (xma_canary_fixes=false) at 4 logical: radio frames lost {ship.get('radio', {}).get('frames_lost')}, "
                       f"mismatch {ship.get('radio', {}).get('mismatch_pct')} %; shipped defaults: {base4.get('radio', {}).get('frames_lost')} lost, "
                       f"{base4.get('radio', {}).get('mismatch_pct')} %")
    problem = base4 is not None and ((steady(base4) or 0) > 0 or late(base4) > 20)
    verdict.append("")
    if base4 is None:
        verdict.append("**No verdict: the 4-logical base run is missing.**")
    elif not problem:
        verdict.append("**Nothing to remove, even at 4 logical processors: no steady underruns and the mixer is never late.** The audio worker does not "
                       "starve on this PC at 4 logical processors, so none of the candidate switches can show a benefit here; they stay off. (A weaker "
                       "single-thread speed than this PC's, or more contention, may still starve it: the player's report is not reproduced.)")
    else:
        verdict.append("**The audio worker is starved at 4 logical processors.** Candidates against e4-base (steady underruns / longest silence / late wakes):")
        winners = []
        for tag in ("e4-prio3", "e4-q16", "e4-adapt", "e4-cap2", "e4-cap1", "e4-best"):
            r = by.get(tag)
            if not r:
                continue
            ok_radio = (r.get("radio", {}).get("frames_lost") in (0, None)) and ((r.get("radio", {}).get("mismatch_pct") or 0) < 1.0)
            gaps_ok = (r.get("mix_gaps") or 0) <= (base4.get("mix_gaps") or 0)
            better = (steady(r) is not None) and (steady(r) <= 0.5 * (steady(base4) or 0)) and late(r) <= late(base4)
            verdict.append(f"  - {tag}: {steady(r)} / {r.get('stats', {}).get('sdl.silence_max')} / {late(r)} late wakes (base {steady(base4)} / "
                           f"{base4.get('stats', {}).get('sdl.silence_max')} / {late(base4)}); mix gaps {r.get('mix_gaps')} (base {base4.get('mix_gaps')}); radio ok: {ok_radio}; "
                           f"FPS {r.get('drive_fps')} (base {base4.get('drive_fps')}) -> " + ("REMOVES UNDERRUNS without a new artefact" if better and ok_radio and gaps_ok else "no"))
            if better and ok_radio and gaps_ok:
                winners.append(tag)
        verdict.append("")
        verdict.append("**Candidates for a default: " + (", ".join(winners) if winners else "none") + ".** Each is still a trade (e4-q16 and e4-adapt cost latency "
                       "after a stall; e4-prio3 takes MMCSS scheduling; the caps change the game's own pacing): decide with the clips and the FPS column, "
                       "and flip a default only with a second run that agrees.")
    cap_lines = []
    for a_, b_ in (("e16-base", "e16-cap2"), ("e16-base", "e16-cap1"), ("e4-base", "e4-cap2"), ("e4-base", "e4-cap1")):
        x, y = by.get(a_), by.get(b_)
        if x and y:
            cap_lines.append(f"- {b_} against {a_}: drive FPS {y.get('drive_fps')} against {x.get('drive_fps')}; pump thread {y.get('pump_thread_hz')} against {x.get('pump_thread_hz')} rounds/s; "
                             f"steady underruns {steady(y)} against {steady(x)}; late wakes {late(y)} against {late(x)}; wake max {y.get('stats', {}).get('wake.max_us')} against {x.get('stats', {}).get('wake.max_us')} us; "
                             f"mixer callback avg {y.get('stats', {}).get('pump.avg_us')} against {x.get('stats', {}).get('pump.avg_us')} us")
    if cap_lines:
        verdict += ["", "## The pump-sleep cap (mcla_pump_sleep_max_ms, the game's thread that sleeps out 7 ms per round)", ""] + cap_lines
        verdict.append("")
        verdict.append("The mixer is paced by the output device, not by that thread; if the steady underruns and the late wakes are the same with the cap, the 7 ms "
                       "sleep is not what delays the mixer.")
    (out / "E-SUMMARY.md").write_text("\n".join(lines + [""] + verdict) + "\n", encoding="utf-8")
    print("\n".join(lines + [""] + verdict))

    if a.clips:
        clips = Path(a.clips)
        clips.mkdir(parents=True, exist_ok=True)
        for tag in ("e4-ship", "e4-best", "e16-base", "e4-base"):
            r = by.get(tag)
            if not r or not r.get("_pair"):
                continue
            batch = r["name"].split("-")[0]
            dump = Path(a.dumps) / r["name"]
            try:
                import subprocess
                subprocess.run([sys.executable, "-P", "-E", str(Path(__file__).resolve().parent / "make_radio_clips.py"), str(dump),
                                str(out / "_ref" / r["name"]), str(clips), f"E-{tag}", "40", "30"], check=False)
                times = aall.log_times(Path(a.runs) / batch / tag / "game.log")
                t = aall.mark_audio_time(times, "title")
                if t is not None and (dump / "mix.wav").exists():
                    aall.write_clip(dump / "mix.wav", t + 6.0, 30.0, clips / f"E-{tag}-title-mix.wav")
            except Exception as e:
                print("clip error", tag, repr(e))


if __name__ == "__main__":
    main()

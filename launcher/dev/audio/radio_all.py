r"""The radio (stereo song = a PAIR of mono XMA contexts) of every dump folder, against FFmpeg's decode of the same input.

   python -P -E radio_all.py --runs <audio\runs> --dumps C:\mcv\audio\dev\dumps --out <results dir> [--tmp <scratch dir>] [--only A-old1,...]

Writes <out>\RADIO.md (one row per run) and <out>\radio-<batch>-<tag>.json.  Light: about 8 s of one core per run (two WAVs of 8 MB,
FFmpeg decodes the .xma twice, FFT correlations).

Columns: frames_lost = whole 512-sample frames the decoder dropped relative to the reference (L+R, estimated from the offset steps);
mismatch_pct = share of 1024-sample blocks more than 64 LSB off the reference (one lost frame spoils everything after it);
first_loss_s = where in the stream the first mismatch is; lr_corr = median correlation of the left and right channel at lag 0 (a song
whose channels are in step: high; channels that drifted apart by whole frames: about 0); off_lag = seconds in which the channels'
best alignment is not 0 (they are shifted against each other); bal_* = left/right level balance in dB per 5 s window.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import radio_pairs as rp  # noqa: E402


def labels(runs_dir):
    out = {}
    for p in Path(runs_dir).glob("*/results.jsonl"):
        for line in p.read_text(encoding="utf-8").splitlines():
            try:
                r = json.loads(line)
            except ValueError:
                continue
            cfg = r.get("cfg", {})
            out[f"{p.parent.name}-{cfg.get('tag')}"] = {"variant": cfg.get("variant"), "aff": cfg.get("aff"), "fps": cfg.get("fps"),
                                                       "exit": r.get("exit_code"), "extra": cfg.get("extra", [])}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", required=True)
    ap.add_argument("--dumps", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tmp", default=None)
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    tmp = Path(a.tmp) if a.tmp else out / "_radio_ref"
    lab = labels(a.runs)
    names = [n for n in a.only.split(",") if n] or sorted(p.name for p in Path(a.dumps).iterdir() if p.is_dir() and not p.name.startswith("S-"))
    cols = ["run", "variant", "aff", "fps", "radio_s", "frames_lost", "mismatch_pct_L", "mismatch_pct_R", "first_loss_s", "lr_corr", "off_lag_s",
            "bal_std_db", "bal_max_abs_db", "bal_step_p95_db", "decode_vs_wall"]
    rows = ["| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    for name in names:
        d = Path(a.dumps) / name
        if not d.is_dir():
            continue
        try:
            pairs = rp.find_pairs(d)
            if not pairs:
                rows.append(f"| {name} | no radio pair found |" + " |" * (len(cols) - 2))
                continue
            pl, pr = pairs[0]
            res = rp.analyze_pair(d, pl, pr, ref=True, tmp=tmp / name)
        except Exception as e:  # one broken dump does not stop the rest
            rows.append(f"| {name} | ERROR {e!r} |" + " |" * (len(cols) - 2))
            continue
        (out / f"radio-{name}.json").write_text(json.dumps(res, indent=1), encoding="utf-8")
        refs = res.get("reference", [{}, {}])
        l = lab.get(name, {})
        xc = res.get("xcorr", {})
        bal = res.get("balance", {})
        firsts = [r.get("first_mismatch_s") for r in refs if (r.get("mismatch_pct") or 0) > 1.0 and r.get("first_mismatch_s") is not None]
        vals = [name, l.get("variant"), l.get("aff"), l.get("fps"), res["seconds"], sum(r.get("frames_lost_est") or 0 for r in refs),
                refs[0].get("mismatch_pct"), refs[1].get("mismatch_pct") if len(refs) > 1 else None, min(firsts) if firsts else "",
                xc.get("lag0_corr_median"), xc.get("windows_off_lag"), bal.get("balance_db_std"), bal.get("balance_db_max_abs"),
                bal.get("balance_step_db_p95"), (res.get("decode_vs_wall") or [None])[0]]
        rows.append("| " + " | ".join("" if v is None else str(v) for v in vals) + " |")
        print(rows[-1], flush=True)
    (out / "RADIO.md").write_text("\n".join(["# The radio pair against the FFmpeg reference", "",
                                              "Columns: " + __doc__.split("Columns:", 1)[1].strip(), "", *rows]) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

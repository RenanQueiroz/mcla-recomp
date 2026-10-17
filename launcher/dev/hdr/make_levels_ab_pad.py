"""Writes a pad script that measures HDR levels on paused frames, the shoulder
before the review fix (mcla_hdr_shoulder_old_tmp) and after it in the same
frame: for day, dusk and night, the 8-bit scene as HDR shows it (SDR
reference) and, per (peak, paper) and highlights setting, a debug-8 capture
with the old and the new shoulder.
The old shoulder needs a dev build with the temporary switch
mcla_hdr_shoulder_old_tmp (hdr-2026-10-16.md, "Second review"); without it the
two captures of a pair are the same.
usage: make_levels_ab_pad.py OUT_DIR PAD_FILE"""
import sys

out, pad = sys.argv[1], sys.argv[2]
times = [("day", "0x41500000"), ("dusk", "0x418C0000"), ("night", "0x41BC0000")]
levels = [(1317, 280), (600, 240)]
highlights = [1, 2]
lines = [
    "set hdr on",
    "wait 8s",
    "press_until start left_title every=2s timeout=90s",
    "wait_until roam 180s",
    "wait 3s",
]
for name, value in times:
    lines += [
        f"log LEVELS {name}",
        f"poke [0x8287E26C]+0x38CC {value}",
        f"poke [0x8287E26C]+0x38DC {value}",
        "wait 4s 120f",
        "press start 4f",
        "wait 2500ms 120f",
    ]
    for peak, paper in levels:
        lines += [
            "set mcla_hdr_scene false",
            f"set hdr_peak_nits {peak}",
            f"set hdr_paper_white_nits {paper}",
            "set hdr_debug_ui 8",
            "wait 300ms 8f",
            f"shot {out}\\{name}_{peak}_{paper}_sdr_d8.png",
            "set mcla_hdr_scene true",
        ]
        for hl in highlights:
            for old in (True, False):
                tag = f"{name}_{peak}_{paper}_h{hl}" + ("_old" if old else "")
                lines += [
                    f"set mcla_hdr_shoulder_old_tmp {'true' if old else 'false'}",
                    f"set mcla_hdr_highlights {hl}",
                    "set hdr_debug_ui 8",
                    "wait 300ms 8f",
                    f"shot {out}\\{tag}_d8.png",
                ]
    lines += [
        "set mcla_hdr_shoulder_old_tmp false",
        "set hdr_debug_ui 0",
        "wait 200ms 4f",
        "press start 4f",
        "wait 2000ms 90f",
    ]
lines.append("log LEVELS end")
with open(pad, "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")

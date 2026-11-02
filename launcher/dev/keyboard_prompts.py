"""Generate an offscreen controller-definition gallery for keyboard-prompt QA.

Requires this branch's native build and a prepared starter-career save in
MCLA_DEV_ROOT/pristine. No physical keyboard or mouse events are sent.
Example (PowerShell, after setting MCLA_DEV_ROOT to an existing devrun root):
  python launcher/dev/keyboard_prompts.py --out D:/agent-work/mcla/prompt-gallery --run

Inspect contact.png against the original controller gallery and manifest.json.
This samples shared icon definitions; it does not play every tutorial or race.
"""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys

# Zero-based frames independently identified from the original Xbox artwork.
# Decorative navigation arrows (24-26) and unused/blank frames are excluded.
CONTROLS = {
    0: "a", 1: "b", 2: "x", 3: "y", 4: "left_shoulder", 5: "right_shoulder",
    6: "left_trigger", 7: "right_trigger", 8: "start", 9: "back",
    12: "lstick_left/lstick_right", 13: "lstick_up/lstick_down/lstick_left/lstick_right",
    14: "rstick_up/rstick_down/rstick_left/rstick_right", 15: "rstick_up/rstick_down",
    16: "rstick_left/rstick_right", 17: "rstick_up/rstick_down/rstick_left/rstick_right",
    18: "dpad_up/dpad_down/dpad_left/dpad_right", 19: "dpad_up/dpad_down",
    20: "dpad_left/dpad_right", 21: "dpad_up/dpad_down/dpad_left/dpad_right",
    22: "left_trigger/right_trigger", 23: "left_shoulder/right_shoulder",
    27: "rstick_press", 28: "lstick_press", 29: "lstick_right", 30: "lstick_left",
    31: "lstick_up", 32: "lstick_down", 33: "rstick_right", 34: "rstick_left",
    35: "rstick_up", 36: "rstick_down",
}


def check_resolved(log, frames):
    text = Path(log).read_text(encoding="utf-8", errors="replace")
    resolved = {int(n) for n in re.findall(r"keyboard prompt: control=(\d+) binding=", text)}
    missing = set(frames) - resolved
    if missing:
        raise RuntimeError(f"Unexercised controller definitions: {sorted(missing)}")
    if "visible=0 foreground=0" not in text or "audio_mute=true" not in text:
        raise RuntimeError("Missing offscreen/mute readback")
    if "SDL input driver initialized" in text or "Unhandled guest access violation" in text:
        raise RuntimeError("Physical SDL input driver or guest crash detected")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--mouse", action="store_true", help="Render MOUSE camera hints; offscreen mode omits physical input drivers")
    parser.add_argument("--playstation", action="store_true", help="Exercise +49 sprite IDs")
    parser.add_argument("--frames", default=",".join(map(str, CONTROLS)))
    args = parser.parse_args()
    try:
        frames = list(dict.fromkeys(int(n) for n in args.frames.split(",")))
    except ValueError:
        parser.error("--frames must be comma-separated integers")
    if not frames or any(n not in CONTROLS for n in frames):
        parser.error("--frames contains an unmapped/decorative frame; see CONTROLS")
    out = args.out.resolve()
    if any(c.isspace() for c in str(out)):
        parser.error("--out must have no whitespace (dev_pad script path tokens)")
    out.mkdir(parents=True, exist_ok=True)
    rows = ["wait 4s", "window state", "get audio_mute mnk_mouse mcla_dev_offscreen",
            "press_until start left_title every=1s timeout=40s", "wait_until roam 60s",
            "wait 4s", "press_until start pause every=2s hold=100ms timeout=25s", "wait 120f"]
    for frame in frames:
        rows += [f"set mcla_key_prompt_gallery_frame {frame+(49 if args.playstation else 0)}", "wait 12f", "wait_until pause 5s",
                 f"shot {out.as_posix()}/{frame:02}.png 0.5"]
    rows += ["set mcla_key_prompt_gallery_frame -1", "window state", "quit"]
    script = out / "gallery.pad"
    script.write_text("\n".join(rows)+"\n", encoding="utf-8")
    (out / "manifest.json").write_text(json.dumps({
        "frame_base": "zero-based Xbox; PlayStation uses +49",
        "source_resolution": [2560, 1440], "capture_scale": 0.5,
        "mouse_hints": args.mouse, "playstation_offset": 49 if args.playstation else 0,
        "frames": [{"frame": n, "controls": CONTROLS[n]} for n in frames],
        "limits": "Starter-career pause-menu fixture, physical input drivers omitted. Inspect screenshots; this is not all-game coverage.",
    }, indent=2)+"\n", encoding="utf-8")
    print(f"Generated {len(frames)} cases: {script}", flush=True)
    if not args.run:
        return
    scratch = Path(os.environ.get("MCLA_DEV_ROOT", ""))
    if not os.environ.get("MCLA_DEV_ROOT") or not (scratch / "pristine").is_dir():
        parser.error("--run requires an existing MCLA_DEV_ROOT/pristine starter-career fixture")
    devrun = Path(__file__).with_name("devrun.py")
    command = [sys.executable, str(devrun), "run", "--offscreen", "--script", str(script),
               "--res", "2", "--fps", "60", "--timeout", "150", "--no-gpu-guard", "--extra",
               "--mcla_keyboard_prompts=true", "--mcla_key_prompt_probe=true", "--mcla_ui_timing_fix=true",
               f"--mnk_mouse={str(args.mouse).lower()}"]
    with (out / "run.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f"Run failed ({result.returncode}); inspect {out / 'run.log'}")
    run_text = (out / "run.log").read_text(encoding="utf-8", errors="replace")
    run_id = re.search(r"(\d{8}-\d{6}): exit 0", run_text)
    if not run_id:
        raise SystemExit("No successful native process outcome in run.log")
    check_resolved(scratch / "runs" / run_id.group(1) / "game.log",
                   [n+(49 if args.playstation else 0) for n in frames])
    from PIL import Image, ImageDraw
    sheet = Image.new("RGB", (4*360, ((len(frames)+3)//4)*130), "#202020")
    draw = ImageDraw.Draw(sheet)
    for i, frame in enumerate(frames):
        with Image.open(out / f"{frame:02}.png") as im:
            if im.size != (1280, 720):
                raise SystemExit(f"Unexpected image dimensions: {im.size}")
            crop = im.crop((375, 465, 545, 515)).resize((340, 100))
        x, y = (i%4)*360, (i//4)*130
        sheet.paste(crop, (x, y+25))
        draw.text((x+5, y+4), f"{frame}: {CONTROLS[frame]}", fill="white")
    sheet.save(out / "contact.png")
    print(f"Completed {len(frames)} cases: {out / 'contact.png'}")


if __name__ == "__main__":
    main()

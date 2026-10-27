r"""Listening clips of the radio defect from the dumps of one run (light: a few WAVs of 8 MB).

   python -P -E make_radio_clips.py <dump dir> <tmp dir with the .ref.pcm files (radio_all.py --tmp)> <out dir> <label> [start_s] [length_s]

Writes, into <out dir>:
  radio-<label>-decoder.wav    the radio pair exactly as the decoder produced it (left context, right context), 44.1 kHz stereo 16-bit
  radio-<label>-reference.wav  the same input decoded by FFmpeg, at the same song time (what a decoder that loses no frame gives)
  radio-<label>-mix.wav        the device mix (mix.wav, before the mute) over the same wall-clock span, folded to stereo
For a run of the shipped decoder the first two differ by whole lost frames (the left and right channel drift against each other
and against the song); for a run with xma_canary_fixes they are the same samples.
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_analyze as aa  # noqa: E402
import radio_pairs as rp  # noqa: E402
import analyze_all as aall  # noqa: E402


def write_stereo(path, left, right, rate):
    n = min(len(left), len(right))
    st = np.stack([left[:n], right[:n]], axis=1)
    k = min(2205, n // 4)
    if k:
        ramp = np.linspace(0, 1, k)[:, None]
        st[:k] *= ramp
        st[-k:] *= ramp[::-1]
    pcm = (np.clip(st, -1, 1) * 32767.0).astype("<i2").tobytes()
    hdr = (b"RIFF" + (36 + len(pcm)).to_bytes(4, "little") + b"WAVEfmt " + (16).to_bytes(4, "little") + (1).to_bytes(2, "little") +
           (2).to_bytes(2, "little") + rate.to_bytes(4, "little") + (rate * 4).to_bytes(4, "little") + (4).to_bytes(2, "little") +
           (16).to_bytes(2, "little") + b"data" + len(pcm).to_bytes(4, "little"))
    Path(path).write_bytes(hdr + pcm)


def main():
    dump, tmp, out, label = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
    start = float(sys.argv[5]) if len(sys.argv) > 5 else 40.0
    length = float(sys.argv[6]) if len(sys.argv) > 6 else 30.0
    out.mkdir(parents=True, exist_ok=True)
    pairs = rp.find_pairs(dump)
    if not pairs:
        print("no radio pair in", dump)
        return 1
    l, r = pairs[0]
    rate = l["rate"]
    a, b = int(start * rate), int((start + length) * rate)
    chans = []
    refs = []
    for c in (l, r):
        _, _, ours = aa.read_wav(dump / (c["stem"] + ".wav"))
        chans.append(ours[a:b, 0])
        res = aa.ref_compare(dump / (c["stem"] + ".wav"), tmp)
        ref = rp.load_ref(tmp / (c["stem"] + ".ref.pcm"))
        lag = res.get("lag_frames", 0)
        refs.append(ref[max(0, a + lag):b + lag])
    write_stereo(out / f"radio-{label}-decoder.wav", chans[0], chans[1], rate)
    write_stereo(out / f"radio-{label}-reference.wav", refs[0], refs[1], rate)
    # the mix over the same wall-clock span: the radio started at l["first_us"] on the audio clock (zero = the dump's start)
    mix = dump / "mix.wav"
    if mix.exists():
        t0 = l["first_us"] / 1e6 + start
        print(aall.write_clip(mix, t0, length, out / f"radio-{label}-mix.wav"))
    print("pair", l["stem"], r["stem"], "from", start, "s for", length, "s ->", out)
    return 0


sys.exit(main())

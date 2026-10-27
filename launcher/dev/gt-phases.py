"""GPU time by render phase, by category and by draw, from the timed frames of the TMPGT/TMPFL instrumentation
(fork-only development tool; the patches are launcher/dev/gpu-timestamps*.sdk.patch, frame-log.sdk.patch and
the two *-split-timestamps patches, applied and built by gt-build.ps1).

  gt-phases.py DIR [--idle-ms 0.5] [--json OUT.json] [--draws] [--top 25] [--label PREFIX] [--min-frames 3]

DIR holds <label>_<n>_gt.csv (a run's tmp_frame_log frames, D3D12) and/or gt_<n>.csv (MCLA_GT_EVERY frames, both
APIs). Each csv: first line `freq,<ticks per second>`, then `i,ticks,label`. Labels:
  FRAME / SWAP         the frame's first and last timestamp
  <x ... >x            a bracket: the work between is category x (xfer = EDRAM ownership transfers and
                       resolve clears, resolve, texload, upload; with the split patches also rdump, rbar, rcopy,
                       rdirect, rclear and the texture-load steps); brackets nest, the innermost counts
  D<n>                 draw n starts (the time to the next label is that draw's, with its state and bindings)
  PASS ...             the render target set changed (a pass boundary)
  M kind=K value=V     the game's marker (rexglue-guest-markers.patch): kind 0x100 = a render phase bit V
                       (--mcla_render_markers_all_phases=true), 3 scene end, 4 tone map, 5 UI, 6 render loop end
Everything between two labels is attributed to the first one's category and to the phase in force. Time between
two labels longer than --idle-ms with no bracket open is idle (the GPU waiting for the CPU thread), not work.
The two tiles of the two-tile scene replay add up. Medians over the frames of a label.

Groups (as frame-structure-2026-10-16.md): pre = phase bits 0x1-0x400 (reflections, environment map, shadow
cascades, impostors), scene = 0x800-0x04000000, post = 0x08000000, tonemap = 0x10000000, hud = 0x20000000.

With --draws and a <label>_<n>.txt text log next to a csv (a frame logged without tmp_frame_log_quiet), draws are
joined to their shaders: time per pixel shader / vertex shader hash, per draw size, and the share of tiny draws.
"""
import collections
import glob
import json
import os
import re
import statistics
import sys

GROUPS = [("pre", 0x1, 0x400), ("scene", 0x800, 0x04000000), ("post", 0x08000000, 0x08000000),
          ("tonemap", 0x10000000, 0x10000000), ("hud", 0x20000000, 0x20000000)]


def group_of(phase):
    if phase is None:
        return "outside"
    for name, lo, hi in GROUPS:
        if lo <= phase <= hi:
            return name
    return "other"


def load(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    freq = int(lines[0].split(",")[1])
    ev = []
    for line in lines[1:]:
        parts = line.split(",", 2)
        if len(parts) == 3:
            ev.append((int(parts[1]) * 1e6 / freq, parts[2]))  # microseconds
    return ev


def analyse(ev, idle_us):
    cats = collections.Counter()
    phases = collections.Counter()        # phase bit -> us
    groups = collections.Counter()
    group_cat = collections.defaultdict(collections.Counter)
    idle = 0.0
    draws = 0
    stack = []
    phase = None
    draw_times = []                       # (n, us, phase)
    cur_draw = None
    for (t0, l0), (t1, l1) in zip(ev, ev[1:]):
        if l0.startswith("<"):
            stack.append(l0[1:])
        elif l0.startswith(">") and stack:
            stack.pop()
        elif l0.startswith("M kind="):
            m = re.match(r"M kind=([0-9A-Fa-f]+) value=([0-9A-Fa-f]+)", l0)
            if m:
                kind, value = int(m.group(1), 16), int(m.group(2), 16)
                if kind == 0x100:
                    phase = value
                elif kind == 6:
                    phase = None
        m = re.fullmatch(r"D(\d+)", l0)
        if m:
            draws += 1
            cur_draw = int(m.group(1))
        dt = max(0.0, t1 - t0)
        cat = stack[-1] if stack else "draw"
        if not stack and dt > idle_us:
            idle += dt
            continue
        cats[cat] += dt
        phases[phase if phase is not None else -1] += dt
        g = group_of(phase)
        groups[g] += dt
        group_cat[g][cat] += dt
        if m and not stack:
            draw_times.append((cur_draw, dt, phase))
    total = sum(cats.values())
    return {"total": total, "idle": idle, "cats": cats, "phases": phases, "groups": groups,
            "group_cat": group_cat, "draws": draws, "draw_times": draw_times}


def med(values):
    return statistics.median(values) if values else 0.0


def parse_text_log(path):
    """{draw n: (prim, cnt, vs, ps)} from the D lines of a frame's text log."""
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.startswith("D n="):
            continue
        f = dict(re.findall(r"(\w+)=([0-9A-Fa-fx.,-]+)", line))
        try:
            out[int(f["n"])] = (int(f["prim"]), int(f["cnt"]), f["vs"], f["ps"])
        except (KeyError, ValueError):
            pass
    return out


def collect(d, label_filter=None):
    sets = collections.defaultdict(list)
    for p in glob.glob(os.path.join(d, "*_gt.csv")) + glob.glob(os.path.join(d, "gt_*.csv")):
        base = os.path.basename(p)
        m = re.fullmatch(r"(.+)_(\d+)_gt\.csv", base)
        if m:
            label = m.group(1)
        elif re.fullmatch(r"gt_\d+\.csv", base):
            label = "gt"
        else:
            continue
        if label_filter and not label.startswith(label_filter):
            continue
        sets[label].append(p)
    return sets


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return
    d = args[0]
    opt = lambda name, default: args[args.index(name) + 1] if name in args else default
    idle_us = float(opt("--idle-ms", "0.5")) * 1000
    top = int(opt("--top", "25"))
    label_filter = opt("--label", None)
    min_frames = int(opt("--min-frames", "3"))
    result = {}
    for label, files in sorted(collect(d, label_filter).items()):
        frames = []
        for p in sorted(files):
            ev = load(p)
            if len(ev) < 50:
                continue
            a = analyse(ev, idle_us)
            a["file"] = p
            frames.append(a)
        if len(frames) < min_frames:
            print(f"(note: {label} has only {len(frames)} usable frame(s))")
        if not frames:
            continue
        n = len(frames)
        r = {"frames": n,
             "gpu_ms": med([f["total"] for f in frames]) / 1000,
             "idle_ms": med([f["idle"] for f in frames]) / 1000,
             "draws": med([f["draws"] for f in frames]),
             "categories_ms": {c: med([f["cats"].get(c, 0.0) for f in frames]) / 1000
                               for c in sorted({c for f in frames for c in f["cats"]})},
             "groups_ms": {g: med([f["groups"].get(g, 0.0) for f in frames]) / 1000
                           for g in ["pre", "scene", "post", "tonemap", "hud", "other", "outside"]},
             "group_categories_ms": {g: {c: med([f["group_cat"][g].get(c, 0.0) for f in frames]) / 1000
                                         for c in sorted({c for f in frames for c in f["group_cat"][g]})}
                                     for g in ["pre", "scene", "post", "tonemap", "hud"]},
             "phase_bits_ms": {("%#x" % b if b >= 0 else "none"): med([f["phases"].get(b, 0.0) for f in frames]) / 1000
                               for b in sorted({b for f in frames for b in f["phases"]})}}
        result[label] = r
        print(f"\n== {label}: {n} frames; GPU busy {r['gpu_ms']:.2f} ms (idle {r['idle_ms']:.2f} ms), "
              f"{r['draws']:.0f} draws")
        print("   groups  : " + "  ".join(f"{g} {v:.2f}" for g, v in r["groups_ms"].items() if v > 0.005))
        print("   category: " + "  ".join(f"{c} {v:.2f}" for c, v in sorted(r["categories_ms"].items(),
                                                                              key=lambda kv: -kv[1]) if v > 0.005))
        print("   phase bits (ms): " + "  ".join(f"{b} {v:.2f}" for b, v in r["phase_bits_ms"].items() if v > 0.005))
        for g in ["pre", "scene", "post", "tonemap", "hud"]:
            gc = r["group_categories_ms"][g]
            if sum(gc.values()) > 0.005:
                print(f"   {g:8s}: " + "  ".join(f"{c} {v:.2f}" for c, v in sorted(gc.items(), key=lambda kv: -kv[1])
                                                 if v > 0.005))
        if "--draws" in args:
            report_draws(frames, files, top, r)
    if "--json" in args:
        with open(opt("--json", "gt-phases.json"), "w", encoding="utf-8") as f:
            json.dump(result, f, indent=1)


def report_draws(frames, files, top, r):
    """Per shader and per size, from frames that also have a text log."""
    per_ps = collections.defaultdict(lambda: [0, 0.0])
    per_vs = collections.defaultdict(lambda: [0, 0.0])
    size_bins = collections.defaultdict(lambda: [0, 0.0])
    tiny = [0, 0.0]
    all_draws = [0, 0.0]
    joined = 0
    for f in frames:
        txt = re.sub(r"_gt\.csv$", ".txt", f["file"])
        if not os.path.exists(txt):
            continue
        info = parse_text_log(txt)
        joined += 1
        for n, us, phase in f["draw_times"]:
            if n not in info:
                continue
            prim, cnt, vs, ps = info[n]
            per_ps[ps][0] += 1
            per_ps[ps][1] += us
            per_vs[vs][0] += 1
            per_vs[vs][1] += us
            b = "<=6 idx" if cnt <= 6 else "<=100" if cnt <= 100 else "<=1000" if cnt <= 1000 else ">1000"
            size_bins[b][0] += 1
            size_bins[b][1] += us
            all_draws[0] += 1
            all_draws[1] += us
            if us < 10.0:
                tiny[0] += 1
                tiny[1] += us
    if not joined:
        print("   (no text-log frames next to the timed ones: run one frame without tmp_frame_log_quiet)")
        return
    print(f"   draws joined from {joined} text-log frame(s): {all_draws[0] // joined} draws/frame, "
          f"{all_draws[1] / joined / 1000:.2f} ms/frame in draws")
    print(f"   draws under 10 us: {tiny[0] / max(1, all_draws[0]) * 100:.0f}% of the draws, "
          f"{tiny[1] / max(1.0, all_draws[1]) * 100:.1f}% of draw time (mean {tiny[1] / max(1, tiny[0]):.1f} us)")
    for b, (c, us) in sorted(size_bins.items()):
        print(f"   size {b:8s}: {c / joined:7.0f} draws/frame {us / joined / 1000:7.3f} ms  mean {us / max(1, c):7.1f} us")
    for title, table in (("pixel shader", per_ps), ("vertex shader", per_vs)):
        print(f"   top {top} by {title} (ms per frame, draws per frame, mean us):")
        for h, (c, us) in sorted(table.items(), key=lambda kv: -kv[1][1])[:top]:
            print(f"     {h}  {us / joined / 1000:7.3f} ms  {c / joined:6.0f} draws  {us / max(1, c):8.1f} us")
    r["draws_joined"] = {"frames": joined, "tiny_draw_share_pct": tiny[0] / max(1, all_draws[0]) * 100,
                         "tiny_time_share_pct": tiny[1] / max(1.0, all_draws[1]) * 100,
                         "per_ps": {h: {"ms": us / joined / 1000, "draws": c / joined}
                                    for h, (c, us) in sorted(per_ps.items(), key=lambda kv: -kv[1][1])[:top]}}


if __name__ == "__main__":
    main()

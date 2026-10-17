"""What a run presented, against what the guest swapped (devrun.py).

A run folder may hold:
  frames.csv     the guest's swaps and the script's marks (src/dev_pad.cpp); its
                 "t0" line puts them on the steady clock (ns; on Windows the
                 performance counter scaled to ns)
  present.csv    the SDK's present log (--present_log, src/ui/presenter.cpp): each
                 guest output refresh ("r", its number; 0 if not published) and each
                 paint ("p": the refresh it showed, the painting thread, the result,
                 how long the paint took); in frame-generation mode on Vulkan the pacer's
                 presents are "p" lines of thread q, and the GPU thread's paints only
                 hand it frames
  presentmon.csv PresentMon's capture of the process (--v2_metrics --qpc_time)

Per segment between marks: swaps, refreshes, the refreshes presented once, more
than once and never, the painting thread, and PresentMon's presents (how many,
how many reached the display, their spacing). Shown frame rates and pacing come
only from presents, never from swap times: the two can differ.

    python presents.py <run folder> [<run folder>...]
"""
import csv
import ctypes
import statistics
import sys
from pathlib import Path


def qpc_frequency():
    if sys.platform != "win32":
        return None
    f = ctypes.c_int64()
    ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(f))
    return f.value


def load_frames(folder):
    """t0 (ns or None), the swap times (ns since t0's epoch), marks [(name, swaps so far)]."""
    t0, swaps, marks = None, [], []
    path = folder / "frames.csv"
    if not path.exists():
        return t0, swaps, marks
    for line in path.read_text(encoding="utf-8").splitlines()[1:]:
        kind, *rest = line.split(",")
        if len(rest) < 3:
            continue
        try:
            if kind == "t0":
                t0 = int(rest[0])
            elif kind == "s":
                swaps.append(int(rest[0]) * 1000)
            elif kind == "mark":
                marks.append((rest[0], int(rest[1])))
        except ValueError:
            pass  # the last line of a game that was killed mid-write
    if t0 is not None:
        swaps = [t0 + s for s in swaps]
    return t0, swaps, marks


def load_present_log(folder):
    refreshes, paints = [], []
    path = folder / "present.csv"
    if not path.exists():
        return None
    for row in csv.reader(path.open(encoding="utf-8")):
        try:
            if row[0] == "r":
                refreshes.append((int(row[1]), int(row[2])))
            elif row[0] == "p":
                paints.append((int(row[1]), int(row[2]), row[3], int(row[4]), int(row[5])))
        except (ValueError, IndexError):
            pass
    return refreshes, paints


def load_presentmon(folder):
    path = folder / "presentmon.csv"
    if not path.exists():
        return None
    rows = list(csv.DictReader(path.open(encoding="utf-8-sig", newline="")))
    if not rows:
        return []
    freq = qpc_frequency()
    time_key = next((k for k in ("CPUStartQPC", "CPUStartTime", "TimeInSeconds") if k in rows[0]), None)
    out = []
    for row in rows:
        if row.get("Application", "").lower() != "mcla.exe":
            continue
        t = row.get(time_key, "")
        try:
            t = float(t)
        except ValueError:
            continue
        ns = int(t * 1e9 / freq) if time_key == "CPUStartQPC" and freq else None
        out.append((ns, row))
    return out


def number(row, *keys):
    for key in keys:
        value = row.get(key)
        if value not in (None, "", "NA"):
            try:
                return float(value)
            except ValueError:
                pass
    return None


def percentiles(values):
    if not values:
        return "-"
    ordered = sorted(values)
    p = lambda q: ordered[min(len(ordered) - 1, int(len(ordered) * q))]
    return f"median {statistics.median(values):.2f}, p1 {p(0.01):.2f}, p99 {p(0.99):.2f}"


def report(folder):
    folder = Path(folder)
    t0, swaps, marks = load_frames(folder)
    log = load_present_log(folder)
    pm = load_presentmon(folder)
    print(f"{folder.name}: {len(swaps)} swaps" + (", present log" if log else "") +
          (f", PresentMon {len(pm)} presents" if pm is not None else ""))
    if t0 is None:
        print("  frames.csv has no t0 line (an older build): no segments by time")
        return
    bounds = [("start", 0)] + marks + [("end", len(swaps))]
    for (name, first), (_, last) in zip(bounds, bounds[1:]):
        if last - first < 2:
            continue
        begin, end = swaps[first] if first < len(swaps) else swaps[-1], swaps[last - 1]
        seconds = (end - begin) / 1e9
        if seconds <= 0:
            continue
        n_swaps = last - first - 1
        line = f"  {name:>12}: {seconds:6.1f} s  swaps {n_swaps / seconds:7.2f}/s"
        details = []
        if log:
            refreshes, paints = log
            seg_r = [n for t, n in refreshes if begin < t <= end]
            published = [n for n in seg_r if n]
            presented = {}
            threads = {"g": 0, "u": 0}
            paint_us = []
            # With a pacer, what was presented is what it presented (and the UI
            # thread's own paints while frame-generation mode is suspended).
            pacer = any(thread == "q" for _, _, thread, _, _ in paints)
            for t, n, thread, result, us in paints:
                presents = thread != "g" if pacer else True
                if presents and begin < t <= end + 50_000_000 and result in (0, 1) and n:
                    presented[n] = presented.get(n, 0) + 1
                if begin < t <= end:
                    threads[thread] = threads.get(thread, 0) + 1
                    paint_us.append(us)
            shown = [n for n in published if n in presented]
            never = len(published) - len(shown)
            repeats = sum(1 for n in shown if presented[n] > 1)
            line += (f"  refreshes {len(seg_r)} (unpublished {len(seg_r) - len(published)})  presented "
                     f"{len(shown)} ({100.0 * len(shown) / max(1, len(published)):.2f}%), never {never}, "
                     f"more than once {repeats}")
            details.append(f"paints: GPU thread {threads.get('g', 0)}, UI thread {threads.get('u', 0)}"
                           + (f", pacer presents {threads.get('q', 0)}" if pacer else "") + "; "
                           f"paint us {percentiles(paint_us)}")
        if pm:
            seg = [row for ns, row in pm if ns is not None and begin < ns <= end]
            if seg:
                displayed = [r for r in seg if number(r, "DisplayLatency", "MsUntilDisplayed") is not None]
                between = [v for v in (number(r, "FrameTime", "MsBetweenPresents") for r in seg)
                           if v is not None]
                change = [v for v in (number(r, "DisplayedTime", "MsBetweenDisplayChange") for r in seg) if v]
                modes = sorted({r.get("PresentMode", "?") for r in seg})
                tearing = sorted({r.get("AllowsTearing", "?") for r in seg})
                line += (f"  presents {len(seg) / seconds:7.2f}/s (x{len(seg) / max(1, n_swaps):.4f} swaps), "
                         f"displayed {100.0 * len(displayed) / len(seg):.1f}%")
                details.append(f"PresentMon: mode {modes}, tearing {tearing}; between presents (FrameTime) ms "
                               f"{percentiles(between)}; displayed for (DisplayedTime) ms {percentiles(change)}")
        print(line)
        for d in details:
            print(f"  {'':>12}  {d}")


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        report(arg)

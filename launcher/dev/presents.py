"""What a run presented, against what the guest swapped (devrun.py).

A run folder may hold:
  frames.csv     the guest's swaps and the script's marks (src/dev_pad.cpp); its
                 "t0" line puts them on the steady clock (ns; on Windows the
                 performance counter scaled to ns)
  present.csv    the SDK's present log (--present_log, src/ui/presenter.cpp): each
                 guest output refresh ("r", its number; 0 if not published) and each
                 paint ("p": the refresh it showed, the painting thread, the result,
                 how long the paint took, the UI frame it showed, 0 for none); in
                 frame-generation mode on Vulkan the pacer's presents are "p" lines of
                 thread q, and the GPU thread's paints only hand it frames; UI overlay
                 painting's suspensions and resumptions ("s": 1 suspended, 0 resumed, 2
                 resumed with no UI to show), and its resume paint (thread r, painted
                 as the GPU thread paints)
  presentmon.csv PresentMon's capture of the process (--v2_metrics --qpc_time)

Per segment between marks: swaps, refreshes, the refreshes presented once, more
than once and never, the painting thread, the paints with UI, and PresentMon's
presents (how many, how many reached the display, their spacing, the GPU's busy
time). Shown frame rates and pacing come only from presents, never from swap
times: the two can differ. Then each suspension and resumption of UI overlay
painting, with the paints around it: any without UI (while there is UI), any
with an older UI frame than one already shown, and on resuming, any guest frame
shown twice.

    python presents.py <run folder> [<run folder>...]
"""
import bisect
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


def load_starts(folder, t0, with_steps=False):
    """The frames' starts (ns on the steady clock): "f" lines of frames.csv, written by the dev
    pad's timer hook right after the low-latency wait; [] without them. With with_steps, the
    simulation step the engine took in each frame (the line's third field, microseconds; 0 in
    files from before it was logged) is returned too, as a second list."""
    path = folder / "frames.csv"
    starts, steps = [], []
    if t0 is None or not path.exists():
        return (starts, steps) if with_steps else starts
    for line in path.read_text(encoding="utf-8").splitlines()[1:]:
        kind, *rest = line.split(",")
        if kind == "f" and rest:
            try:
                starts.append(t0 + int(rest[0]) * 1000)
                steps.append(int(rest[1]) if len(rest) > 1 else 0)
            except ValueError:
                pass
    return (starts, steps) if with_steps else starts


def load_present_log(folder):
    refreshes, paints, switches = [], [], []
    path = folder / "present.csv"
    if not path.exists():
        return None
    for row in csv.reader(path.open(encoding="utf-8")):
        try:
            if row[0] == "r":
                refreshes.append((int(row[1]), int(row[2])))
            elif row[0] == "p":
                # The UI column is missing in logs of older builds.
                ui = int(row[6]) if len(row) > 6 else None
                paints.append((int(row[1]), int(row[2]), row[3], int(row[4]), int(row[5]), ui))
            elif row[0] == "s":
                switches.append((int(row[1]), int(row[2])))
        except (ValueError, IndexError):
            pass
    return refreshes, paints, switches


def latencies(log, pm, begin, end, starts=None, swaps=None, steps=None):
    """Each real frame's time from its swap (the refresh's log line) to the screen (PresentMon's
    present of it: its CPU start plus DisplayLatency). The frame's present is found from its paint
    line: thread c (D3D12's frame generation callback, logged before the present) takes the first
    present called after it, the others (logged after their present) the last one before it."""
    refreshes, paints, _ = log
    refresh_time = {n: t for t, n in refreshes if n}
    # A frame's start: the engine timer pass whose frame the swap (the guest's VdSwap, the first
    # one after the pass) ends; the GPU thread processes the swaps in the same order, so the
    # refresh at the same position in the log is that swap's (a refresh made by the GPU thread
    # can come after the next frame's start when it lags).
    start_of_refresh = {}
    step_of_refresh = {}
    if starts and swaps:
        order = [n for _, n in refreshes]
        for j, start in enumerate(starts):
            k = bisect.bisect_right(swaps, start)
            if k < len(swaps) and k < len(order) and order[k]:
                start_of_refresh[order[k]] = start
                if steps and steps[j]:
                    step_of_refresh[order[k]] = steps[j] / 1000.0
    calls = []
    for ns, row in pm:
        busy = number(row, "CPUBusy")
        latency = number(row, "DisplayLatency", "MsUntilDisplayed")
        if ns is None or busy is None or latency is None:
            continue
        calls.append((ns + int(busy * 1e6), ns + int(latency * 1e6)))
    calls.sort()
    times = [c for c, _ in calls]
    pacer = any(thread in ("q", "c") for _, _, thread, _, _, _ in paints)
    # Where PresentMon's present of a frame comes: Direct3D 12's is called before the paint's log
    # line, Vulkan's (made by the driver's own thread) just after it. Taken from where the nearest
    # present falls for the paints of the run.
    after = before = 0
    # (With a pacer, the GPU thread's "g" lines only hand frames over: the pacer's own presents
    # are the ones to look at.)
    voters = ("q",) if pacer else ("g", "u")
    for t, n, thread, _, _, _ in paints[len(paints) // 3:]:
        if after + before >= 400:
            break
        if thread in voters and times:
            k = bisect.bisect_left(times, t)
            after_gap = times[k] - t if k < len(times) else None
            before_gap = t - times[k - 1] if k > 0 else None
            if after_gap is not None and (before_gap is None or after_gap < before_gap):
                after += 1
            else:
                before += 1
    present_after_log = after > before
    result = []
    from_start = []
    shown_at = {}
    seen = set()
    for t, n, thread, status, _, _ in paints:
        if not (begin < t <= end) or not n or n in seen or n not in refresh_time or status < 0:
            continue
        if pacer and thread not in ("q", "c"):
            continue
        if thread == "c":
            # Direct3D 12's frame generation callback runs just after the proxy's present of the
            # generated frame returned and before the real frame's: the first present after it.
            i = bisect.bisect_left(times, t)
        elif present_after_log:
            i = bisect.bisect_left(times, t - 500_000)
        else:
            i = bisect.bisect_right(times, t) - 1
        if i < 0 or i >= len(calls) or abs(calls[i][0] - t) > 50_000_000:
            continue
        seen.add(n)
        result.append((calls[i][1] - refresh_time[n]) / 1e6)
        shown_at[n] = calls[i][1]
        # From the frame's start (the last engine timer pass before its swap) to the screen.
        if n in start_of_refresh and refresh_time[n] - start_of_refresh[n] < 200_000_000:
            from_start.append((calls[i][1] - start_of_refresh[n]) / 1e6)
    if steps is not None:
        # The motion the engine's step promised against the one on the screen: for each frame
        # shown right after the one before it, the time between their display times minus the
        # step the engine took for it (ms). Zero everywhere is perfectly even motion; the
        # spread is the judder.
        errors = []
        step_values = []
        for n, shown in shown_at.items():
            if n - 1 in shown_at and n in step_of_refresh:
                errors.append((shown - shown_at[n - 1]) / 1e6 - step_of_refresh[n])
                step_values.append(step_of_refresh[n])
        return result, from_start, errors, step_values
    return (result, from_start) if starts is not None else result


def report_switches(t0, paints, switches):
    """UI overlay painting's suspensions and resumptions, with the paints around each."""
    if not switches:
        return
    print(f"  UI overlay painting: suspended {sum(1 for _, k in switches if k == 1)} times, "
          f"resumed {sum(1 for _, k in switches if k != 1)}")
    shown = [x for x in paints if x[2] not in ("q", "c")]
    for t, kind in switches:
        name = {1: "suspended", 0: "resumed", 2: "resumed (no UI)"}.get(kind, str(kind))
        window = [x for x in shown if t - 100_000_000 <= x[0] <= t + 300_000_000]
        with_ui = kind != 2
        blank = older = 0
        newest = 0
        for _, _, _, _, _, ui in window:
            if ui is None:
                continue
            if with_ui and ui == 0 and newest:
                blank += 1
            if ui and ui < newest:
                older += 1
            newest = max(newest, ui or 0)
        twice = 0
        if kind != 1:
            seen = set()
            for pt, n, thread, result, _, _ in window:
                if pt >= t and n and result in (0, 1):
                    twice += n in seen
                    seen.add(n)
        threads = {}
        for _, _, thread, _, _, _ in window:
            threads[thread] = threads.get(thread, 0) + 1
        at = f"{(t - t0) / 1e9:7.2f} s" if t0 else str(t)
        print(f"    {name:>16} at {at}: paints from 100 ms before to 300 ms after "
              f"{dict(sorted(threads.items()))}; without UI {blank}, with an older UI frame {older}"
              + (f", guest frames shown twice after it {twice}" if kind != 1 else ""))


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
    starts, steps = load_starts(folder, t0, with_steps=True)
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
            refreshes, paints, _ = log
            seg_r = [n for t, n in refreshes if begin < t <= end]
            published = [n for n in seg_r if n]
            presented = {}
            threads = {"g": 0, "u": 0}
            paint_us = []
            # With a pacer, what was presented is what it presented (and the UI
            # thread's own paints while frame-generation mode is suspended).
            pacer = any(thread in ("q", "c") for _, _, thread, _, _, _ in paints)
            # The refreshes a pacer (q) or D3D12's frame generation callback (c) presented: the
            # GPU thread's and the resume (r) paints of those only hand them frames.
            paced = {n for _, n, thread, _, _, _ in paints if thread in ("q", "c")}
            drops = 0
            ui_paints = with_ui = 0
            for t, n, thread, result, us, ui in paints:
                # The UI thread's own paints present.
                presents = not (thread in ("g", "r") and n in paced)
                if begin < t <= end and thread not in ("q", "c") and ui is not None:
                    ui_paints += 1
                    with_ui += ui > 0
                # PaintResult for threads g and u (presented, presented suboptimal);
                # VkResult for the pacer (VK_SUCCESS, VK_SUBOPTIMAL_KHR), -1 for a frame
                # it dropped.
                ok = result in ((0, 1000001003) if thread in ("q", "c") else (0, 1))
                if presents and begin < t <= end + 50_000_000 and ok and n:
                    presented[n] = presented.get(n, 0) + 1
                if begin < t <= end:
                    if thread in ("q", "c") and result == -1:
                        drops += 1
                        continue
                    threads[thread] = threads.get(thread, 0) + 1
                    paint_us.append(us)
            shown = [n for n in published if n in presented]
            never = len(published) - len(shown)
            repeats = sum(1 for n in shown if presented[n] > 1)
            line += (f"  refreshes {len(seg_r)} (unpublished {len(seg_r) - len(published)})  presented "
                     f"{len(shown)} ({100.0 * len(shown) / max(1, len(published)):.2f}%), never {never}, "
                     f"more than once {repeats}")
            details.append(f"paints: GPU thread {threads.get('g', 0)}, UI thread {threads.get('u', 0)}"
                           + (f" (resume {threads['r']})" if threads.get("r") else "")
                           + (f", pacer presents {threads.get('q', 0) + threads.get('c', 0)}, drops "
                              f"{drops}" if pacer else "")
                           + (f", with UI {with_ui} of {ui_paints}" if ui_paints else "")
                           + "; "
                           f"paint us {percentiles(paint_us)}")
        if pm and log:
            lat, from_start, step_errors, step_values = latencies(log, pm, begin, end, starts,
                                                                 swaps, steps)
            if lat:
                details.append(f"real frames' swap to screen ms {percentiles(lat)} ({len(lat)})")
            if from_start:
                details.append(f"real frames' start (engine timer) to screen ms {percentiles(from_start)} "
                               f"({len(from_start)})")
            if len(step_errors) > 20:
                ordered = sorted(abs(e) for e in step_errors)
                details.append(
                    f"engine step vs display interval ms: step median "
                    f"{statistics.median(step_values):.2f} sigma {statistics.pstdev(step_values):.3f}; "
                    f"error mean {statistics.mean(step_errors):+.3f} sigma "
                    f"{statistics.pstdev(step_errors):.3f} p99 abs {ordered[int(len(ordered) * 0.99)]:.2f} "
                    f"({len(step_errors)})")
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
                # The process's GPU work between one present and the next (PresentMon 2's GPUBusy):
                # its share of the segment, and per swap of the game.
                busy = [v for v in (number(r, "GPUBusy", "MsGPUActive") for r in seg) if v is not None]
                if busy:
                    details.append(f"GPU busy {sum(busy) / (seconds * 10.0):.1f}% of the time, "
                                   f"{sum(busy) / max(1, n_swaps):.3f} ms per swap; per present ms "
                                   f"{percentiles(busy)}")
        print(line)
        for d in details:
            print(f"  {'':>12}  {d}")
    if log:
        report_switches(t0, log[1], log[2])


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        report(arg)

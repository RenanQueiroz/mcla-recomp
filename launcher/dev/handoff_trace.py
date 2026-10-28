r"""Reads a hand-off trace (a .hotrace file written by the game with --handoff_trace) and says where the threads wait for each other.

  python -I handoff_trace.py <trace.hotrace> [--gen <generated\default folder>] [--threads <threads.csv>] [--window LABEL]
                             [--frames N] [--json out.json] [--timeline N] [--min-wait-us 10]

The trace is the SDK's rex::handoff buffer (patches\rexglue-handoff-trace.patch: guest kernel waits and signals with the
guest call chain of the caller, the command processor's waits, the Direct3D 12 submission thread) plus the game's own
events (src\handoff_trace.h: the engine timer, the swap, the ring-slot wait of the update thread, the thread that frees
the slots, the render thread's loop). File format (little endian), version 1:
   header   64 bytes   "HOTRACE1", u32 version, u32 slot_size (40), u64 count, u32 threads, u32 labels,
                       i64 start_ns, i64 dump_ns, u64 dropped, u64 reserved
   threads  56 bytes   u32 tid, u32 guest handle, i64 first_ns, char name[40]
   labels   48 bytes   char label[48] (label 0 is "")
   events   40 bytes   u64 t (steady_clock ns), u16 event, u16 thread, u32 a, b, c, u32 ra[4] (guest return addresses)

What it prints, per window (a stretch recorded between two marks of handoff_trace_run, named by handoff_trace_label; the
whole file when it has none):
   1. the frames: how many, the update thread's period (median, p95, worst) and the swaps;
   2. for every thread that waits: the time a frame spends in each wait (by the guest call site), how often, who ended it;
   3. who waits for whom: waiter thread -> thread that ended its wait, microseconds a frame;
   4. the critical path of a frame: from the end of a frame back through the waits that held the update thread (a wait ended
      by another thread's signal moves the path to that thread, at the time of the signal: the time between the signal and
      the wake-up is the wake-up latency, kept on the waiter), as a share of the frame, averaged, then the path of the
      median frame and of the worst one;
   5. the hand-off numbers: the update thread's wait for a ring slot against the freeing thread's round, the command
      processor's idle and busy time, the render thread's frame and its wait for work.
A wait is blocking from --min-wait-us (10) microseconds; a shorter one counts as running.
The guest call chain is shown as function names when --gen is given (the generated\default folder of a build: the names
come from its mcla_funcs.*.h), else as addresses. --threads takes the threads.csv of ab_batch.py (tid, name) to name threads
the trace could not name.
"""
import argparse
import bisect
import collections
import csv
import json
import re
import statistics
import struct
import sys
from operator import itemgetter
from pathlib import Path

EV_NAMES = {
    1: "mark", 10: "wait_begin", 11: "wait_end", 12: "wait_poll", 13: "wait_multi_begin", 14: "wait_multi_end",
    15: "signal_and_wait", 16: "event_set", 17: "sem_release", 18: "mutant_release", 19: "delay_begin", 20: "delay_end",
    40: "cp_idle_begin", 41: "cp_idle_end", 42: "cp_exec_begin", 43: "cp_exec_end", 44: "cp_wptr", 45: "cp_swap_begin",
    46: "cp_swap_end", 47: "d3d_fence_begin", 48: "d3d_fence_end", 49: "submit_queue", 50: "submit_begin", 51: "submit_end",
    52: "submit_await_begin", 53: "submit_await_end", 54: "cp_waitreg_begin", 55: "cp_waitreg_end",
    100: "frame_start", 101: "swap", 102: "limiter_begin", 103: "limiter_end", 110: "ring_begin", 112: "ring_end",
    113: "ring_advance", 120: "pump_round", 121: "pump_free", 122: "pump_sleep_begin", 123: "pump_sleep_end",
    124: "pump_wake_set", 130: "render_loop", 131: "render_frame", 132: "kick", 133: "wait_frame_done", 134: "game_loop",
}
EV = {v: k for k, v in EV_NAMES.items()}
OBJ_TYPES = {0: "?", 1: "enumerator", 2: "event", 3: "file", 4: "iocompletion", 5: "module", 6: "mutant", 7: "notify",
             8: "semaphore", 9: "session", 10: "socket", 11: "symlink", 12: "thread", 13: "timer"}
# Guest functions that only pass a wait on: the call site is the first frame outside them.
WRAPPERS = {"sub_82135DC0", "sub_82135DB8", "sub_821C90C0", "sub_821C9078", "sub_821C9108", "sub_8244ED10", "sub_82460270",
            "sub_821C91B8", "sub_8244FEC0"}
# Blocking kinds: a wait that another thread's signal, or the clock, ends.
BLOCKING = {"wait", "multi", "delay", "ring", "pump_sleep", "cp_idle", "d3d_fence", "cp_waitreg", "submit_await", "limiter"}


class Trace:
    pass


def load(path):
    raw = Path(path).read_bytes()
    magic, version, slot_size, count, nthr, nlab, start_ns, dump_ns, dropped, _ = struct.unpack_from("<8sIIQIIqqQQ", raw, 0)
    if magic != b"HOTRACE1" or version != 1 or slot_size != 40:
        raise SystemExit(f"{path}: not a version 1 hand-off trace")
    off = 64
    threads = []
    for i in range(nthr):
        tid, handle, first, name = struct.unpack_from("<IIq40s", raw, off)
        off += 56
        threads.append({"idx": i, "tid": tid, "handle": handle, "first_ns": first,
                        "name": name.split(b"\0")[0].decode("utf-8", "replace")})
    labels = []
    for i in range(nlab):
        labels.append(raw[off + 48 * i: off + 48 * (i + 1)].split(b"\0")[0].decode("utf-8", "replace"))
    off += 48 * nlab
    rows = [r for r in struct.iter_unpack("<QHHIII4I", memoryview(raw)[off: off + 40 * count]) if r[1] != 0]
    rows.sort(key=itemgetter(0))
    t = Trace()
    t.threads, t.labels, t.start_ns, t.dump_ns, t.dropped, t.count = threads, labels, start_ns, dump_ns, dropped, int(count)
    t.rows = rows  # (t ns, event, thread, a, b, c, ra0, ra1, ra2, ra3), by time
    t.first_us = (rows[0][0] - start_ns) / 1000.0 if rows else 0.0
    t.last_us = (rows[-1][0] - start_ns) / 1000.0 if rows else 0.0
    return t


def load_functions(folder):
    """Sorted guest function starts from mcla_funcs.*.h of a generated\\default folder."""
    starts = []
    for p in Path(folder).glob("mcla_funcs*.h"):
        for m in re.finditer(r"DECLARE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)", p.read_text(encoding="utf-8", errors="replace")):
            starts.append(int(m.group(1), 16))
    starts = sorted(set(starts))
    return starts


class Symbols:
    def __init__(self, starts):
        self.starts = starts

    def name(self, addr):
        if not addr:
            return ""
        if not self.starts:
            return f"{addr:08X}"
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0 or addr - self.starts[i] > 0x8000:
            return f"{addr:08X}"
        off = addr - self.starts[i]
        return f"sub_{self.starts[i]:08X}" + (f"+{off:#x}" if off else "")

    def func(self, addr):
        n = self.name(addr)
        return n.split("+")[0]


def read_threads_csv(path):
    names = {}
    if not path:
        return names
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                if row.get("name"):
                    names[int(row["tid"])] = row["name"]
            except (KeyError, ValueError):
                pass
    return names


def roles_of(tr):
    """Thread index -> role, from which events it records."""
    cnt = collections.defaultdict(collections.Counter)
    for r in tr.rows:
        if r[1] in (100, 130, 120, 42, 50):
            cnt[r[1]][r[2]] += 1
    roles = {}
    for code, role in ((100, "update"), (130, "render"), (120, "pump"), (42, "gpu-commands"), (50, "gpu-submission")):
        if cnt[code]:
            th = cnt[code].most_common(1)[0][0]
            if th not in roles:
                roles[th] = role
    return roles


class Span:
    __slots__ = ("thr", "kind", "b", "e", "obj", "ra", "info", "waker", "wake_t", "id")

    def __init__(self, thr, kind, b, e, obj=None, ra=None, info=None):
        self.thr, self.kind, self.b, self.e, self.obj, self.ra, self.info = thr, kind, b, e, obj, ra, info
        self.waker = None
        self.wake_t = None
        self.id = 0


def build(tr):
    """Walk the events in time order: spans (waits, busy stretches), signals by key, points by kind."""
    spans = []
    signals = collections.defaultdict(list)   # key -> [(t, thr, ra)]
    points = collections.defaultdict(list)    # event name -> [(t, thr, a, b)]
    marks = []                                # (t, label, start?)
    open_ = {}
    start_ns = tr.start_ns
    for row in tr.rows:
        t = (row[0] - start_ns) / 1000.0
        e, th, a, b, c = row[1], row[2], row[3], row[4], row[5]
        ra = row[6:10]
        if e == 1:
            marks.append((t, a, b))
        elif e == 10:
            open_[th] = ("wait", t, a, ra, (c, b))
        elif e == 11:
            o = open_.pop(th, None)
            if o and o[0] == "wait":
                spans.append(Span(th, "wait", o[1], t, ("h", o[2]), o[3], {"type": o[4][0], "timeout": o[4][1], "result": b}))
        elif e == 12:
            spans.append(Span(th, "poll", t, t, ("h", a), ra, {"type": c, "result": b}))
        elif e == 13:
            open_[th] = ("multi", t, a, ra, (b, c))
        elif e == 14:
            o = open_.pop(th, None)
            if o and o[0] == "multi":
                spans.append(Span(th, "multi", o[1], t, ("h", o[2]), o[3], {"count": o[4][0] & 0xFFFF, "all": (o[4][0] >> 16) == 0}))
        elif e == 15:
            signals[("h", a)].append((t, th, ra))
            open_[th] = ("wait", t, b, ra, (c, 0xFFFFFFFF))
        elif e in (16, 17, 18):
            signals[("h", a)].append((t, th, ra))
        elif e == 19:
            open_[th] = ("delay", t, a, ra, None)
        elif e == 20:
            o = open_.pop(th, None)
            if o and o[0] == "delay":
                spans.append(Span(th, "delay", o[1], t, None, o[3], {"ms": o[2]}))
        elif e == 40:
            open_[th] = ("cp_idle", t, a, ra, None)
        elif e == 41:
            o = open_.pop(th, None)
            if o and o[0] == "cp_idle":
                spans.append(Span(th, "cp_idle", o[1], t, ("wptr",), None, {"turns": a, "write": b}))
        elif e == 42:
            open_[th] = ("cp_exec", t, a, ra, b)
        elif e == 43:
            o = open_.pop(th, None)
            if o and o[0] == "cp_exec":
                spans.append(Span(th, "cp_exec", o[1], t, None, None, {"read": o[2], "write": o[4], "new_read": a}))
        elif e == 44:
            signals[("wptr",)].append((t, th, ra))
            points["cp_wptr"].append((t, th, a, b))
        elif e == 45:
            open_[th] = ("cp_swap", t, a, ra, None)
        elif e == 46:
            o = open_.pop(th, None)
            if o and o[0] == "cp_swap":
                spans.append(Span(th, "cp_swap", o[1], t, None, None, {}))
        elif e == 47:
            open_[th] = ("d3d_fence", t, a, ra, None)
        elif e == 48:
            o = open_.pop(th, None)
            if o and o[0] == "d3d_fence":
                spans.append(Span(th, "d3d_fence", o[1], t, None, None, {"frame": o[2]}))
        elif e == 49:
            points["submit_queue"].append((t, th, a, b))
        elif e == 50:
            open_[th] = ("submit", t, a, ra, None)
        elif e == 51:
            signals[("submitdone",)].append((t, th, ra))
            o = open_.pop(th, None)
            if o and o[0] == "submit":
                spans.append(Span(th, "submit", o[1], t, None, None, {"n": o[2]}))
        elif e == 52:
            open_[th] = ("submit_await", t, a, ra, None)
        elif e == 53:
            o = open_.pop(th, None)
            if o and o[0] == "submit_await":
                spans.append(Span(th, "submit_await", o[1], t, ("submitdone",), None, {}))
        elif e == 54:
            open_[th] = ("cp_waitreg", t, a, ra, b)
        elif e == 55:
            o = open_.pop(th, None)
            if o and o[0] == "cp_waitreg":
                spans.append(Span(th, "cp_waitreg", o[1], t, None, None, {"addr": o[2]}))
        elif e == 102:
            open_[th] = ("limiter", t, a, ra, b)
        elif e == 103:
            o = open_.pop(th, None)
            if o and o[0] == "limiter":
                spans.append(Span(th, "limiter", o[1], t, None, None, {"site": o[2], "us": o[4]}))
        elif e == 110:
            open_[th] = ("ring", t, a, ra, None)
        elif e == 112:
            o = open_.pop(th, None)
            if o and o[0] == "ring":
                spans.append(Span(th, "ring", o[1], t, ("slot",), None, {"turns": a, "waited": b, "object": o[2]}))
        elif e == 113:
            points["ring_advance"].append((t, th, a, b))
            signals[("ring_advance",)].append((t, th, ra))
        elif e == 121:
            signals[("slot",)].append((t, th, ra))
            points["pump_free"].append((t, th, a, b))
        elif e == 122:
            open_[th] = ("pump_sleep", t, a, ra, None)
        elif e == 123:
            o = open_.pop(th, None)
            if o and o[0] == "pump_sleep":
                spans.append(Span(th, "pump_sleep", o[1], t, ("pumpwake",) if a else None, None, {"ms": o[2], "early": a}))
        elif e == 124:
            signals[("pumpwake",)].append((t, th, ra))
            points["pump_wake_set"].append((t, th, a, b))
        elif e in EV_NAMES:
            points[EV_NAMES[e]].append((t, th, a, b))
    # waits still open at the end of the trace are cut there (they ran to the end)
    end_t = tr.last_us
    for th, o in open_.items():
        if o[0] in BLOCKING:
            spans.append(Span(th, o[0], o[1], end_t, None, o[3], {"cut": True}))
    spans.sort(key=lambda s: (s.e, s.b))
    for n, s in enumerate(spans):
        s.id = n
    for k in signals:
        signals[k].sort(key=lambda x: x[0])
    return spans, signals, points, marks


def resolve_wakers(spans, signals):
    """For every blocking span with an object: the latest signal of another thread between its start and its end."""
    keys = {k: [x[0] for x in v] for k, v in signals.items()}
    for s in spans:
        if s.kind not in BLOCKING or s.obj is None or s.info.get("cut"):
            continue
        lst = signals.get(s.obj)
        if not lst:
            continue
        ts = keys[s.obj]
        j = bisect.bisect_right(ts, s.e) - 1
        # a signal recorded up to 200 us before the wait's own record may still have been the one that ended it: the
        # signalling thread can be descheduled between recording the signal and setting the object
        while j >= 0 and ts[j] >= s.b - 200.0:
            t, th, ra = lst[j]
            if th != s.thr:
                s.waker, s.wake_t = th, t
                break
            j -= 1


class Report:
    def __init__(self, tr, args):
        self.tr, self.args = tr, args
        gen = args.gen
        if not gen:
            guess = Path(__file__).resolve().parents[2] / "generated" / "default"
            gen = str(guess) if guess.is_dir() else ""
        starts = load_functions(gen) if gen else []
        self.sym = Symbols(starts)
        self.csv_names = read_threads_csv(args.threads)
        self.roles = roles_of(tr)
        self.spans, self.signals, self.points, self.marks = build(tr)
        resolve_wakers(self.spans, self.signals)
        self.by_thread = collections.defaultdict(list)
        for s in self.spans:
            self.by_thread[s.thr].append(s)
        self.ends = {t: [s.e for s in lst] for t, lst in self.by_thread.items()}

    # ---- names
    def tname(self, idx):
        if idx is None:
            return "(outside)"
        info = self.tr.threads[idx] if idx < len(self.tr.threads) else {"name": "", "tid": 0, "handle": 0}
        name = info["name"] or self.csv_names.get(info["tid"], "")
        if not name and info["handle"]:
            name = f"({info['handle']:08X})"
        if not name:
            name = f"tid {info['tid']}"
        role = self.roles.get(idx)
        return f"{role}[{name}]" if role else name

    def site(self, ra):
        """The call site of a wait: the first two guest frames outside the wait wrappers."""
        frames = [a for a in (ra or []) if a]
        if not frames:
            return ""
        keep = [a for a in frames if self.sym.func(a) not in WRAPPERS]
        pick = keep[:2] if keep else frames[:1]
        return " < ".join(self.sym.name(a) for a in pick)

    def span_label(self, s):
        k = s.kind
        if k == "wait":
            typ = OBJ_TYPES.get(s.info.get("type", 0), "?")
            return f"{typ} {s.obj[1]:08X} at {self.site(s.ra)}"
        if k == "multi":
            return f"{s.info['count']} objects from {s.obj[1]:08X} at {self.site(s.ra)}"
        if k == "delay":
            return f"Sleep({s.info['ms']}) at {self.site(s.ra)}"
        if k == "ring":
            return "ring slot (sub_82148A10)"
        if k == "pump_sleep":
            return "pump sleep " + ("(ended early by the update thread)" if s.info.get("early") else "(ran out)")
        if k == "cp_idle":
            return "idle (waiting for the write pointer)"
        if k == "d3d_fence":
            return "Direct3D 12 frame fence"
        if k == "cp_waitreg":
            return f"WAIT_REG_MEM {s.info['addr']:#x}"
        if k == "submit_await":
            return "await the submission thread"
        if k == "limiter":
            return f"frame-rate limiter ({'swap' if s.info.get('site') else 'timer'})"
        return k

    # ---- windows
    def windows(self):
        out = []
        open_ = {}
        for t, label, start in self.marks:
            if start:
                open_[label] = t
            elif label in open_:
                out.append((self.tr.labels[label] if label < len(self.tr.labels) else str(label), open_.pop(label), t))
        for label, t in open_.items():
            out.append((self.tr.labels[label] if label < len(self.tr.labels) else str(label), t, self.tr.last_us))
        if not out:
            out = [("all", self.tr.first_us, self.tr.last_us)]
        if self.args.window:
            out = [w for w in out if w[0] == self.args.window]
        return sorted(out, key=lambda w: w[1])

    def update_thread(self):
        for k, v in self.roles.items():
            if v == "update":
                return k
        return None

    def points_in(self, name, lo, hi, thr=None):
        return [p for p in self.points.get(name, []) if lo <= p[0] < hi and (thr is None or p[1] == thr)]

    # ---- sections
    def blocking_in(self, thr, lo, hi):
        return [s for s in self.by_thread.get(thr, []) if s.kind in BLOCKING and s.e > lo and s.b < hi and s.e - s.b >= self.args.min_wait_us]

    def wait_budget(self, lo, hi, nframes, w):
        """Per thread: blocking time by wait, per frame."""
        rows = []
        thr_total = {}
        for thr in sorted(self.by_thread):
            agg = collections.defaultdict(lambda: [0.0, 0, collections.Counter()])
            total = 0.0
            for s in self.blocking_in(thr, lo, hi):
                d = min(s.e, hi) - max(s.b, lo)
                key = (s.kind, self.span_label(s))
                a = agg[key]
                a[0] += d
                a[1] += 1
                a[2][s.waker] += d
                total += d
            if total > 0:
                thr_total[thr] = total
                rows.append((thr, total, agg))
        rows.sort(key=lambda r: -r[1])
        w(f"  Blocking waits per frame, by thread (a thread's time in waits of {self.args.min_wait_us:g} us or more; the rest of its frame is running or spinning):")
        shown = 0
        for thr, total, agg in rows:
            role = self.roles.get(thr)
            if shown >= 10 and not role:
                continue
            shown += 1
            w(f"   {self.tname(thr)}: {total / nframes / 1000:.3f} ms a frame in waits")
            for (kind, label), (d, n, wk) in sorted(agg.items(), key=lambda kv: -kv[1][0])[: self.args.sites]:
                who = ", ".join(f"{self.tname(k)} {v / nframes:.0f}" for k, v in wk.most_common(2) if k is not None)
                ext = wk.get(None, 0.0)
                w(f"      {d / nframes / 1000:7.3f} ms  {n / nframes:6.1f} x  {label}" + (f"   ended by: {who}" if who else "")
                  + (f"   (no thread: {ext / nframes:.0f} us)" if ext and who else ""))
        return thr_total

    def who_waits_for_whom(self, lo, hi, nframes, w):
        agg = collections.defaultdict(float)
        for thr in self.by_thread:
            for s in self.blocking_in(thr, lo, hi):
                if s.waker is not None:
                    agg[(thr, s.waker)] += min(s.e, hi) - max(s.b, lo)
        w("  Who waits for whom (waiter -> the thread whose signal ended the wait; microseconds a frame; waits of other threads' signals only):")
        for (a, b), d in sorted(agg.items(), key=lambda kv: -kv[1])[:12]:
            w(f"      {self.tname(a):<34} -> {self.tname(b):<34} {d / nframes:8.0f} us")

    # ---- critical path
    def last_blocking_before(self, thr, t, lo):
        lst = self.by_thread.get(thr, [])
        ends = self.ends.get(thr, [])
        j = bisect.bisect_right(ends, t) - 1
        while j >= 0:
            s = lst[j]
            if s.e <= lo:
                return None
            if s.kind in BLOCKING and s.e - s.b >= self.args.min_wait_us:
                return s
            j -= 1
        return None

    def critical_path(self, lo, hi, thr):
        """Segments (thread, kind, label, begin, end, waker) from `lo` to `hi`, in time order."""
        path = []
        t = hi
        used = set()
        guard = 0
        while t > lo + 0.5 and guard < 400:
            guard += 1
            s = self.last_blocking_before(thr, t, lo)
            if s is None or s.id in used:
                path.append((thr, "run", "running", lo, t, None))
                break
            used.add(s.id)
            if s.e < t:
                path.append((thr, "run", "running", s.e, t, None))
            if s.waker is not None and s.wake_t is not None and s.wake_t <= s.e:
                if s.e > s.wake_t:
                    path.append((thr, "wake", f"wake-up after {self.tname(s.waker)} ({self.span_label(s)})", s.wake_t, s.e, s.waker))
                thr, t = s.waker, max(lo, s.wake_t)
            else:
                path.append((thr, s.kind, self.span_label(s), max(s.b, lo), s.e, None))
                t = max(lo, s.b)
        path.reverse()
        return path

    def critical_section(self, lo, hi, w):
        upd = self.update_thread()
        if upd is None:
            w("  (no engine timer events: no critical path)")
            return
        starts = [p[0] for p in self.points_in("frame_start", lo, hi, upd)]
        if len(starts) < 3:
            w("  (fewer than 3 frames)")
            return
        frames = list(zip(starts[:-1], starts[1:]))
        total = collections.defaultdict(float)
        per_frame = []
        for lo_f, hi_f in frames:
            path = self.critical_path(lo_f, hi_f, upd)
            per_frame.append((hi_f - lo_f, path))
            for thr, kind, label, b, e, wk in path:
                if kind == "run":
                    key = (self.tname(thr), "running")
                elif kind == "wake":
                    key = (self.tname(thr), "wake-up latency, after " + self.tname(wk))
                else:
                    key = (self.tname(thr), f"{kind}: {label}")
                total[key] += e - b
        n = len(frames)
        dur = statistics.mean(d for d, _ in per_frame)
        w(f"  Critical path of the update thread's frame, average of {n} frames of {dur / 1000:.3f} ms (the path from the end of a frame back through the waits that held it):")
        for (who, what), v in sorted(total.items(), key=lambda kv: -kv[1])[:14]:
            w(f"      {v / n / 1000:7.3f} ms  {100 * v / n / dur:5.1f}%   {who}: {what}")
        order = sorted(range(n), key=lambda i: per_frame[i][0])
        for name, i in (("median", order[n // 2]), ("p95", order[min(n - 1, int(n * 0.95))]), ("longest", order[-1])):
            d, path = per_frame[i]
            w(f"  The {name} frame ({d / 1000:.3f} ms, starting {frames[i][0] / 1e6:.4f} s into the trace), its critical path:")
            merged = []
            for seg in path:
                if merged and merged[-1][0] == seg[0] and merged[-1][1] == seg[1] and merged[-1][2] == seg[2]:
                    merged[-1] = (seg[0], seg[1], seg[2], merged[-1][3], seg[4], seg[5])
                else:
                    merged.append(seg)
            for thr, kind, label, b, e, wk in merged:
                if e - b >= self.args.min_wait_us:
                    w(f"      +{(b - frames[i][0]) / 1000:7.3f} ms  {(e - b) / 1000:6.3f} ms  {self.tname(thr)}: {label}")
        return frames, per_frame

    # ---- hand-off numbers
    def handoff_section(self, lo, hi, nframes, w):
        upd = self.update_thread()
        w("  Hand-off numbers:")
        rings = [s for s in self.spans if s.kind == "ring" and s.e > lo and s.b < hi and s.info.get("waited")]
        allrings = [s for s in self.spans if s.kind == "ring" and s.e > lo and s.b < hi]
        if allrings:
            d = [s.e - s.b for s in rings]
            w(f"      ring-slot waits (sub_82148A10 with at least one Sleep(0)): {len(rings) / nframes:.2f} a frame, "
              f"{sum(d) / nframes / 1000:.3f} ms a frame" + (f", median {statistics.median(d):.0f} us, p95 {sorted(d)[int(len(d) * .95)]:.0f} us" if d else "")
              + f"; {len(allrings) / nframes:.1f} calls a frame, {statistics.mean(s.info['turns'] for s in rings) if rings else 0:.0f} turns a wait")
            lat_set, lat_free, lat_resume = [], [], []
            sets = self.signals.get(("pumpwake",), [])
            frees = self.signals.get(("slot",), [])
            sts, fts = [x[0] for x in sets], [x[0] for x in frees]
            rounds = [p[0] for p in self.points.get("pump_round", [])]
            for s in rings:
                i = bisect.bisect_left(sts, s.b - 2.0)
                if i < len(sts) and sts[i] <= s.e:
                    set_t = sts[i]
                    j = bisect.bisect_left(rounds, set_t)
                    k = bisect.bisect_left(fts, set_t)
                    if j < len(rounds) and k < len(fts) and fts[k] <= s.e:
                        lat_set.append(rounds[j] - set_t)
                        lat_free.append(fts[k] - rounds[j])
                        lat_resume.append(s.e - fts[k])
            if lat_set:
                w(f"      of those, with the pump thread woken by the update thread ({len(lat_set)}): event set -> round starts {statistics.median(lat_set):.0f} us (median), "
                  f"round starts -> slot freed {statistics.median(lat_free):.0f} us, slot freed -> the update thread leaves its wait {statistics.median(lat_resume):.0f} us")
            adv = [p[0] for p in self.points.get("ring_advance", []) if lo <= p[0] < hi]
            if adv and allrings:
                # the gap from publishing a slot to the next call of the wait: room for the pump to run in the background
                begs = sorted(s.b for s in allrings)
                gaps = []
                for t_adv in adv:
                    j = bisect.bisect_left(begs, t_adv)
                    if j < len(begs):
                        gaps.append(begs[j] - t_adv)
                if gaps:
                    w(f"      from the update thread publishing a slot to its next call of the wait: median {statistics.median(gaps):.0f} us, p5 {sorted(gaps)[int(len(gaps) * .05)]:.0f} us, p95 {sorted(gaps)[int(len(gaps) * .95)]:.0f} us")
        prs = [p[0] for p in self.points_in("pump_round", lo, hi)]
        if len(prs) > 1:
            w(f"      pump thread: {len(prs) / nframes:.2f} rounds a frame, period median {statistics.median(b - a for a, b in zip(prs, prs[1:])):.0f} us")
            ps = [s for s in self.spans if s.kind == "pump_sleep" and s.e > lo and s.b < hi]
            if ps:
                early = sum(1 for s in ps if s.info.get("early"))
                w(f"      its waits: {len(ps)}, {early} ended early by the update thread, {len(ps) - early} ran out (median {statistics.median(s.e - s.b for s in ps):.0f} us)")
        # command processor
        cp = next((k for k, v in self.roles.items() if v == "gpu-commands"), None)
        if cp is not None:
            idle = sum(min(s.e, hi) - max(s.b, lo) for s in self.by_thread.get(cp, []) if s.kind == "cp_idle" and s.e > lo and s.b < hi)
            exe = sum(min(s.e, hi) - max(s.b, lo) for s in self.by_thread.get(cp, []) if s.kind == "cp_exec" and s.e > lo and s.b < hi)
            fence = sum(min(s.e, hi) - max(s.b, lo) for s in self.by_thread.get(cp, []) if s.kind == "d3d_fence" and s.e > lo and s.b < hi)
            wreg = sum(min(s.e, hi) - max(s.b, lo) for s in self.by_thread.get(cp, []) if s.kind == "cp_waitreg" and s.e > lo and s.b < hi)
            await_ = sum(min(s.e, hi) - max(s.b, lo) for s in self.by_thread.get(cp, []) if s.kind == "submit_await" and s.e > lo and s.b < hi)
            span = hi - lo
            w(f"      GPU command thread: idle (no commands) {100 * idle / span:.0f}% of the time, executing {100 * exe / span:.0f}%, "
              f"waiting for the D3D12 fence {100 * fence / span:.1f}%, WAIT_REG_MEM {100 * wreg / span:.1f}%, for the submission thread {100 * await_ / span:.1f}%")
            wp = self.points_in("cp_wptr", lo, hi)
            idles = sorted((s for s in self.by_thread.get(cp, []) if s.kind == "cp_idle" and lo <= s.e < hi), key=lambda s: s.e)
            ie = [s.e for s in idles]
            lat = []
            for t_w, th, a_, b_ in wp:
                j = bisect.bisect_left(ie, t_w)
                if j < len(ie) and idles[j].b < t_w:
                    lat.append(ie[j] - t_w)
            if lat:
                w(f"      write pointer set by the guest -> the command processor leaves its idle wait: median {statistics.median(lat):.0f} us, p95 {sorted(lat)[int(len(lat) * .95)]:.0f} us ({len(lat)} of {len(wp)} writes found it idle)")
        # render thread
        rt = next((k for k, v in self.roles.items() if v == "render"), None)
        if rt is not None:
            loops = [p[0] for p in self.points_in("render_loop", lo, hi, rt)]
            frames_ = [p[0] for p in self.points_in("render_frame", lo, hi)]
            if frames_:
                w(f"      render thread: {len(frames_) / nframes:.2f} frame functions a frame, {len(loops) / nframes:.2f} loop turns a frame")
            if len(loops) > 1 and frames_:
                waits = [s for s in self.by_thread.get(rt, []) if s.kind in ("wait", "multi") and s.e > lo and s.b < hi and s.e - s.b >= self.args.min_wait_us]
                tw = sum(min(s.e, hi) - max(s.b, lo) for s in waits)
                w(f"      render thread: in waits {100 * tw / (hi - lo):.0f}% of the time ({tw / nframes / 1000:.3f} ms a frame)")
        kicks = self.points_in("kick", lo, hi)
        if kicks:
            w(f"      the update thread started the render thread's frame {len(kicks) / nframes:.2f} times a frame")

    # ---- a timeline of one frame
    def timeline(self, lo, hi, w, limit=140):
        rows = []
        for thr, lst in self.by_thread.items():
            for s in lst:
                if s.e > lo and s.b < hi and s.kind in BLOCKING and s.e - s.b >= self.args.min_wait_us * 2:
                    who = f", ended by {self.tname(s.waker)}" if s.waker is not None else ""
                    rows.append((s.b, f"{self.tname(thr)} waits {(s.e - s.b) / 1000:.3f} ms: {self.span_label(s)}{who}"))
        for name in ("frame_start", "swap", "kick", "wait_frame_done", "render_frame", "render_loop", "ring_advance", "pump_round", "pump_free", "pump_wake_set", "cp_wptr"):
            for t, th, a, b in self.points.get(name, []):
                if lo <= t < hi:
                    rows.append((t, f"{self.tname(th)} {name}"))
        for thr, lst in self.by_thread.items():
            for s in lst:
                if s.kind in ("cp_exec", "cp_swap", "submit") and s.e > lo and s.b < hi and s.e - s.b >= 50:
                    rows.append((s.b, f"{self.tname(thr)} {s.kind} for {(s.e - s.b) / 1000:.3f} ms"))
        rows.sort()
        for t, text in rows[:limit]:
            w(f"      +{(t - lo) / 1000:7.3f} ms  {text}")
        if len(rows) > limit:
            w(f"      ... {len(rows) - limit} more")

    def run(self, out):
        def w(s=""):
            out.append(s)
        tr = self.tr
        w(f"hand-off trace: {tr.count} events ({len(tr.rows)} complete), {tr.dropped} dropped, {len(tr.threads)} threads, "
          f"{(tr.dump_ns - tr.start_ns) / 1e9:.1f} s between the start and the dump; windows: " +
          ", ".join(f"{n} {(e - b) / 1e6:.1f} s" for n, b, e in self.windows()))
        w("  threads: " + "; ".join(f"{self.tname(i)}" for i in range(len(tr.threads)) if i in self.roles))
        upd = self.update_thread()
        summary = {}
        for name, lo, hi in self.windows():
            w("")
            w(f"==== window {name}: {(hi - lo) / 1e6:.2f} s ====")
            starts = [p[0] for p in self.points_in("frame_start", lo, hi, upd)] if upd is not None else []
            swaps = self.points_in("swap", lo, hi)
            nframes = max(1, len(starts) - 1)
            if len(starts) > 2:
                per = [b - a for a, b in zip(starts, starts[1:])]
                ps = sorted(per)
                w(f"  update thread: {len(starts)} engine timer passes, period median {statistics.median(per) / 1000:.3f} ms, p95 {ps[int(len(ps) * .95)] / 1000:.3f}, "
                  f"worst {ps[-1] / 1000:.3f} ms ({1e6 * len(per) / sum(per):.0f} a second); {len(swaps)} swaps")
                summary[name] = {"frames": len(per), "fps": 1e6 * len(per) / sum(per), "period_median_ms": statistics.median(per) / 1000,
                                 "period_p95_ms": ps[int(len(ps) * .95)] / 1000}
            else:
                w(f"  update thread: {len(starts)} engine timer passes ({len(swaps)} swaps): too few for a frame structure")
            w("")
            thr_total = self.wait_budget(lo, hi, nframes, w)
            w("")
            self.who_waits_for_whom(lo, hi, nframes, w)
            w("")
            res = self.critical_section(lo, hi, w)
            w("")
            self.handoff_section(lo, hi, nframes, w)
            if upd is not None and res and self.args.timeline:
                frames, per_frame = res
                order = sorted(range(len(frames)), key=lambda i: per_frame[i][0])
                i = order[len(order) // 2]
                w("")
                w(f"  Timeline of the median frame ({per_frame[i][0] / 1000:.3f} ms; waits of {self.args.min_wait_us * 2:g} us or more, the points of the hand-off):")
                self.timeline(frames[i][0], frames[i][1], w)
            if name in summary and upd is not None:
                u = sum(min(s.e, hi) - max(s.b, lo) for s in self.blocking_in(upd, lo, hi))
                summary[name]["update_wait_ms_per_frame"] = u / nframes / 1000
                summary[name]["update_wait_share"] = u / (hi - lo)
                if res:
                    frames, per_frame = res
                    comp = collections.defaultdict(float)
                    for d, path in per_frame:
                        for thr, kind, label, b, e, wk in path:
                            comp[("running" if kind == "run" else "wake" if kind == "wake" else kind) + ":" + self.roles.get(thr, "other")] += e - b
                    tot = sum(comp.values())
                    summary[name]["critical_path_share"] = {k: v / tot for k, v in sorted(comp.items(), key=lambda kv: -kv[1])[:8]}
        return summary


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--gen", default="", help="generated\\default folder (function names)")
    ap.add_argument("--threads", default="", help="threads.csv of ab_batch.py (names for threads the trace could not name)")
    ap.add_argument("--window", default="", help="only this window (a handoff_trace_label)")
    ap.add_argument("--min-wait-us", type=float, default=10.0)
    ap.add_argument("--sites", type=int, default=5, help="wait sites listed per thread")
    ap.add_argument("--timeline", type=int, default=1, help="print the timeline of the median frame (0: not)")
    ap.add_argument("--json", default="", help="write a summary for A/B comparisons")
    args = ap.parse_args()
    tr = load(args.trace)
    rep = Report(tr, args)
    out = []
    summary = rep.run(out)
    print("\n".join(out))
    if args.json:
        Path(args.json).write_text(json.dumps(summary, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()

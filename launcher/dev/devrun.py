"""Unattended mcla.exe runs with scripted controller input (fork-only dev tool).

Every run plays on a scratch copy of the save data, never on the real one:

  devrun.py prepare            copy Documents\\mcla (save, achievements) to a
                               pristine snapshot under %LOCALAPPDATA%\\mcla-dev
  devrun.py run --script scripts\\boot-to-roam.pad [--backend vulkan] [--res 3]
  devrun.py bench --script scripts\\boot-to-roam.pad --configs d3d12:1,vulkan:1 --runs 3
  devrun.py bench --script scripts\\race.pad --configs d3d12:1,d3d12:3 --runs 5 --fixed-dt 60
  devrun.py analyze <run folder or frames.csv>
  devrun.py record <drive.csv> [--fps 120] [--fullscreen]
                               play on the save copy with your own controller;
                               your inputs are recorded from the moment you are
                               in the city (close the game to stop)
  devrun.py gpu [--seconds 10] which other processes use the GPU right now

Before each run the scratch copy is restored from the snapshot and mcla.toml
(next to mcla.exe, where the game saves its settings) is backed up; after the
run mcla.toml is put back, and the real save data and mcla.toml are checked
to be byte-identical to before the run.

GPU guard: during every run, once a second, the GPU use of every other process
(Windows' "GPU Engine" counters, 3D and compute engines) is sampled; a process
other than the game, its children and dwm.exe above --gpu-threshold percent
over the run or over any 5 s marks the run as contaminated (gpu-guard.json in
the run folder). bench redoes such runs (--gpu-retries) and leaves them out.
"""

import argparse
import ctypes
import hashlib
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import threading
import time
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
# MCLA_EXE: another build to run (an A/B against a copy of an older one); its
# folder holds its DLLs and, if any, its mcla.toml.
EXE = Path(os.environ.get("MCLA_EXE") or REPO / "out" / "build" / "win-amd64-release" / "mcla.exe")
TOML = EXE.with_name("mcla.toml")
REAL_USER = Path(os.environ["USERPROFILE"]) / "Documents" / "mcla"
# MCLA_DEV_ROOT: another scratch folder (save copy, shader storage, runs), for a
# second checkout whose runs must not share one with this one.
DEV = Path(os.environ.get("MCLA_DEV_ROOT") or Path(os.environ["LOCALAPPDATA"]) / "mcla-dev")
# MCLA_GAME_DATA: the extracted game, for a checkout whose game\ folder holds only
# default.xex (for codegen).
GAME_DATA = os.environ.get("MCLA_GAME_DATA", "")
PRISTINE = DEV / "pristine"
WORK = DEV / "work"
CACHE = DEV / "cache"
RUNS = DEV / "runs"
# MCLA_PRESENTMON: PresentMon's console program (--presentmon); by default the
# one winget installs (Intel.PresentMon.Console). Without elevation it needs the
# account in "Performance Log Users".
PRESENTMON = os.environ.get("MCLA_PRESENTMON") or next(iter(sorted(
    (Path(os.environ["LOCALAPPDATA"]) / "Microsoft" / "WinGet" / "Packages").glob(
        "Intel.PresentMon.Console_*/presentmon*.exe"))), None)


def real_files():
    # The shader cache too: runs use their own (--cache_root), so it must not change either.
    files = [p for p in REAL_USER.rglob("*") if p.is_file()]
    if TOML.exists():
        files.append(TOML)
    return sorted(files)


def fingerprint():
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in real_files()}


def prepare(refresh=False):
    if PRISTINE.exists() and not refresh:
        return
    if PRISTINE.exists():
        shutil.rmtree(PRISTINE)
    shutil.copytree(REAL_USER, PRISTINE, ignore=shutil.ignore_patterns("cache"))
    print(f"snapshot of {REAL_USER} in {PRISTINE}")


def restore_work():
    if WORK.exists():
        shutil.rmtree(WORK)
    shutil.copytree(PRISTINE, WORK)
    CACHE.mkdir(parents=True, exist_ok=True)


user32 = ctypes.WinDLL("user32", use_last_error=True)
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def close_windows(pid):
    """Ask the game to close (WM_CLOSE), as clicking the window's X does."""
    def callback(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        return True
    user32.EnumWindows(WNDENUMPROC(callback), 0)


# GPU guard. Another program on the GPU (a second game, a video, a stress test)
# takes GPU time from the game and skews its frame times. Windows counts each
# process's time on every GPU engine ("\GPU Engine(*)\Utilization Percentage",
# instances pid_<n>_luid_..._eng_<k>_engtype_<type>); the guard reads them
# through PDH, which expands the wildcard again at every collection, so
# processes that start or exit mid-run come and go. A read takes about 1 ms.
GPU_COUNTER = r"\GPU Engine(*)\Utilization Percentage"
# Engine types counted, as substrings: "3D" also matches "High Priority 3D";
# Compute_0/1, Graphics_1 and Cuda where a GPU has them (this RTX 5080 lists
# 3D, Copy, video, Security, OFA, JPEG and VR engines, no compute ones). Copy,
# video and the rest are left out: a browser playing a video uses VideoDecode.
GPU_ENGINES = ("3D", "Compute", "Graphics", "Cuda")
# dwm.exe composes the game's own window, and its use rises with the game's.
GPU_IGNORE = {"dwm.exe"}
GPU_THRESHOLD = 5.0   # percent of one engine, over the run or any GPU_WINDOW
GPU_WINDOW = 5.0      # seconds
GPU_INTERVAL = 1.0    # seconds between samples

PDH_MORE_DATA = 0x800007D2
PDH_NO_DATA = 0x800007D5
PDH_FMT_DOUBLE_NOCAP100 = 0x00000200 | 0x00008000


class _PdhValue(ctypes.Structure):
    _fields_ = [("CStatus", wintypes.DWORD), ("doubleValue", ctypes.c_double)]


class _PdhItem(ctypes.Structure):
    _fields_ = [("szName", wintypes.LPWSTR), ("FmtValue", _PdhValue)]


class GpuCounters:
    """One PDH query on the GPU Engine counters; read() gives each process's use
    of the counted engines since the previous read."""

    def __init__(self):
        pdh = self.pdh = ctypes.WinDLL("pdh")
        handle_out = ctypes.POINTER(ctypes.c_void_p)
        for name, argtypes in (
                ("PdhOpenQueryW", [wintypes.LPCWSTR, ctypes.c_size_t, handle_out]),
                ("PdhAddEnglishCounterW", [ctypes.c_void_p, wintypes.LPCWSTR, ctypes.c_size_t,
                                           handle_out]),
                ("PdhCollectQueryData", [ctypes.c_void_p]),
                ("PdhGetFormattedCounterArrayW", [ctypes.c_void_p, wintypes.DWORD,
                                                  ctypes.POINTER(wintypes.DWORD),
                                                  ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]),
                ("PdhCloseQuery", [ctypes.c_void_p])):
            f = getattr(pdh, name)
            f.argtypes, f.restype = argtypes, wintypes.DWORD
        self.query, self.counter = ctypes.c_void_p(), ctypes.c_void_p()
        self.check(pdh.PdhOpenQueryW(None, 0, ctypes.byref(self.query)), "PdhOpenQuery")
        status = pdh.PdhAddEnglishCounterW(self.query, GPU_COUNTER, 0, ctypes.byref(self.counter))
        if status:
            pdh.PdhCloseQuery(self.query)
            self.check(status, f"PdhAddEnglishCounter {GPU_COUNTER}")
        self.keys = {}  # instance name -> (pid, counted engine?)
        self.lock = threading.Lock()
        self.last = time.monotonic()
        status = pdh.PdhCollectQueryData(self.query)  # the first collection: rates need two
        if status not in (0, PDH_NO_DATA):
            self.check(status, "PdhCollectQueryData")

    @staticmethod
    def check(status, what):
        if status:
            raise OSError(f"{what}: PDH status 0x{status:08x}")

    def key(self, name):
        key = self.keys.get(name)
        if key is None:
            pid = None
            if name.startswith("pid_"):
                try:
                    pid = int(name[4:name.index("_", 4)])
                except ValueError:
                    pass
            engine = name.rpartition("engtype_")[2]
            key = self.keys[name] = (pid, any(e in engine for e in GPU_ENGINES))
            if len(self.keys) > 50000:  # instance names of processes long gone
                self.keys.clear()
        return key

    def read(self):
        """(seconds since the previous read, {pid: percent}). Every process with a GPU
        context is listed, also at 0; the percentages add up its counted engines."""
        with self.lock:
            pdh = self.pdh
            status = pdh.PdhCollectQueryData(self.query)
            now = time.monotonic()
            covered, self.last = now - self.last, now
            if status == PDH_NO_DATA:
                return covered, {}
            self.check(status, "PdhCollectQueryData")
            size, count, buf = wintypes.DWORD(0), wintypes.DWORD(0), None
            for _ in range(4):
                status = pdh.PdhGetFormattedCounterArrayW(
                    self.counter, PDH_FMT_DOUBLE_NOCAP100, ctypes.byref(size),
                    ctypes.byref(count), buf)
                if status != PDH_MORE_DATA:
                    break
                buf = (ctypes.c_byte * size.value)()
            if status == PDH_NO_DATA or (status == 0 and buf is None):
                return covered, {}
            self.check(status, "PdhGetFormattedCounterArray")
            items = ctypes.cast(buf, ctypes.POINTER(_PdhItem))
            usage = {}
            for i in range(count.value):
                item = items[i]
                pid, counted = self.key(item.szName)
                if pid is None:
                    continue
                # Valid or new data; an instance seen for the first time has none yet.
                ok = item.FmtValue.CStatus in (0, 1)
                value = item.FmtValue.doubleValue if counted and ok else 0.0
                usage[pid] = usage.get(pid, 0.0) + value
            return covered, usage


_gpu_counters = None  # GpuCounters, or the reason they are not available


def gpu_counters():
    """The process's one GpuCounters, or None (with a warning the first time)."""
    global _gpu_counters
    if _gpu_counters is None:
        try:
            _gpu_counters = GpuCounters()
        except OSError as e:
            _gpu_counters = str(e)
            print(f"GPU guard: the GPU Engine counters are not available ({e}); "
                  "runs are not checked for other processes using the GPU")
    return _gpu_counters if isinstance(_gpu_counters, GpuCounters) else None


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
kernel32.Process32FirstW.argtypes = [ctypes.c_void_p, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32NextW.argtypes = [ctypes.c_void_p, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = ctypes.c_void_p
kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.TerminateProcess.argtypes = [ctypes.c_void_p, wintypes.UINT]


def process_table():
    """{pid: (parent pid, exe name)} of every running process; needs no rights."""
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if snap is None or snap == ctypes.c_void_p(-1).value:
        return {}
    table = {}
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            table[entry.th32ProcessID] = (entry.th32ParentProcessID, entry.szExeFile)
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return table


def follow_restarts(pid, deadline):
    """The pause menu's RESTART GAME starts mcla.exe again, as a child of the
    one that then quits. Follow each restart until the last one exits, so the
    run's checks and the restore of mcla.toml come after it; one still running
    at the deadline is closed. Returns the restarted processes' pids."""
    restarts = []
    while True:
        kids = [child for child, (parent, name) in process_table().items()
                if parent == pid and name.lower() == "mcla.exe"]
        if not kids:
            return restarts
        pid = kids[0]
        restarts.append(pid)
        print(f"the game started again as process {pid} (RESTART GAME); following it")
        handle = kernel32.OpenProcess(0x00100000 | 0x0001, False, pid)  # SYNCHRONIZE | TERMINATE
        if not handle:
            continue
        try:
            remaining = max(0.0, deadline - time.monotonic())
            if kernel32.WaitForSingleObject(handle, int(remaining * 1000)) != 0:
                print(f"timeout; closing process {pid}")
                close_windows(pid)
                if kernel32.WaitForSingleObject(handle, 30_000) != 0:
                    kernel32.TerminateProcess(handle, 1)
        finally:
            kernel32.CloseHandle(handle)


def end_games(proc):
    """Closes whatever of this run's games still runs (the one started, and
    any it started again), as the window's close button does, then by force."""
    pids = []
    if proc.poll() is None:
        pids.append(proc.pid)
    table = process_table()
    family = game_pids(proc.pid, {pid: info[0] for pid, info in table.items()})
    pids += [pid for pid in family if pid != proc.pid and table.get(pid, (0, ""))[1].lower() == "mcla.exe"]
    for pid in pids:
        handle = kernel32.OpenProcess(0x00100000 | 0x0001, False, pid)  # SYNCHRONIZE | TERMINATE
        if not handle:
            continue
        try:
            if kernel32.WaitForSingleObject(handle, 0) != 0:
                print(f"closing process {pid} of this run")
                close_windows(pid)
                if kernel32.WaitForSingleObject(handle, 30_000) != 0:
                    kernel32.TerminateProcess(handle, 1)
                    kernel32.WaitForSingleObject(handle, 10_000)
        finally:
            kernel32.CloseHandle(handle)


def game_pids(root, parents, before=()):
    """root and every process whose chain of parents ({pid: parent pid}) leads
    to it. A process already there before the game started (`before`) is not
    its child, even if a dead parent's pid was given to the game."""
    pids = {root}
    grew = root is not None
    while grew:
        grew = False
        for pid, parent in parents.items():
            if parent in pids and pid not in pids and pid not in before:
                pids.add(pid)
                grew = True
    pids.discard(None)
    return pids


def gpu_assess(samples, names, game, threshold=GPU_THRESHOLD, window=GPU_WINDOW,
               interval=GPU_INTERVAL):
    """A run's GPU guard verdict from samples [(seconds covered, {pid: percent})].
    names: {pid: exe name}; game: the game's pids. Per process: the mean over the
    run (weighted by the time each sample covers; a process absent from a sample,
    e.g. one that has exited, counts as 0) and the highest mean over any `window`
    seconds (that many samples; the whole run if shorter). A process other than
    the game's and GPU_IGNORE above the threshold in either is an offender."""
    total = sum(dt for dt, _ in samples)
    report = {"threshold": threshold, "window_s": window, "samples": len(samples),
              "seconds": round(total, 1)}
    if not samples or total <= 0:
        report["clean"] = None
        return report
    if not any(u for _, u in samples):  # not even dwm.exe: the counters saw nothing
        report["clean"] = None
        report["error"] = "the GPU Engine counters returned no data"
        return report
    k = max(1, round(window / interval))
    ignore = {n.lower() for n in GPU_IGNORE}
    procs = []
    for pid in sorted(set().union(*(u for _, u in samples))):
        series = [(dt, u.get(pid, 0.0)) for dt, u in samples]
        worst = 0.0
        for end in range(min(k, len(series)), len(series) + 1):
            part = series[max(0, end - k):end]
            span = sum(dt for dt, _ in part)
            if span > 0:
                worst = max(worst, sum(dt * v for dt, v in part) / span)
        name = names.get(pid, "?")
        procs.append({
            "pid": pid, "name": name,
            "role": "game" if pid in game else "ignored" if name.lower() in ignore else "other",
            "mean": round(sum(dt * v for dt, v in series) / total, 2),
            "window": round(worst, 2),
            "peak": round(max(v for _, v in series), 2),
        })
    game_series = [sum(v for pid, v in u.items() if pid in game) for _, u in samples]
    others = [p for p in procs if p["role"] == "other"]
    offenders = [p for p in others if p["mean"] > threshold or p["window"] > threshold]
    report.update({
        "clean": not offenders,
        "offenders": sorted(offenders, key=lambda p: -p["window"]),
        "game": {"pids": sorted(game),
                 "mean": round(sum(dt * v for (dt, _), v in zip(samples, game_series)) / total, 2),
                 "peak": round(max(game_series), 2)},
        "busiest_others": sorted((p for p in others if p not in offenders),
                                 key=lambda p: -p["window"])[:3],
        "processes": [p for p in procs if p["peak"] >= 0.1 or p["role"] == "game"],
    })
    return report


def describe(procs, window=GPU_WINDOW):
    return ", ".join(f"{p['name']} (pid {p['pid']}) {p['mean']:.1f}% mean, "
                     f"{p['window']:.1f}% over {window:g} s, peak {p['peak']:.0f}%" for p in procs)


def gpu_summary(r):
    """One line on a run's GPU guard report."""
    if r is None:
        return "GPU guard: off"
    if r.get("clean") is None:
        return f"GPU guard: not checked ({r.get('error') or 'no samples'})"
    w = r["window_s"]
    if r["clean"]:
        line = f"GPU guard: clean, no other process above {r['threshold']:g}%"
        if r["busiest_others"]:
            p = r["busiest_others"][0]
            line += f" (busiest {p['name']} {p['mean']:.1f}% mean, {p['window']:.1f}% over {w:g} s)"
    else:
        line = f"GPU guard: CONTAMINATED by {describe(r['offenders'], w)}"
    if r["game"]["pids"]:
        line += f"; the game {r['game']['mean']:.0f}% mean"
    if r.get("busy_at_start"):
        line += ("; busy before the start: " +
                 ", ".join(f"{p['name']} (pid {p['pid']}) {p['mean']:.0f}%" for p in r["busy_at_start"]))
    return line


def gpu_report(out):
    """A run folder's gpu-guard.json, or None."""
    path = Path(out) / "gpu-guard.json"
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


class GpuGuard:
    """Samples every process's GPU use once a second on a thread while the game runs."""

    def __init__(self, threshold=GPU_THRESHOLD):
        self.threshold = threshold
        self.counters = gpu_counters()
        self.procs = {}       # pid -> (parent pid, exe name), as last seen appearing
        self.present = set()  # pids in the previous read
        self.before = set()   # pids seen before the game started
        self.samples = []
        self.errors = []
        self.busy_at_start = []
        self.waited = 0.0
        self.game_pid = None
        self.thread = None
        self.stopping = threading.Event()
        self.read_seconds = 0.0

    def read(self):
        covered, usage = self.counters.read()
        new = set(usage) - self.present
        if new:  # resolve names and parents while the processes still exist
            table = process_table()
            for pid in new:
                if pid in table:
                    self.procs[pid] = table[pid]
        self.present = set(usage)
        return covered, usage

    def busy(self, seconds=2):
        """Other processes above the threshold over the next `seconds` (1 s reads)."""
        samples = []
        for _ in range(seconds):
            time.sleep(GPU_INTERVAL)
            samples.append(self.read())
        self.before |= self.present
        names = {pid: info[1] for pid, info in self.procs.items()}
        r = gpu_assess(samples, names, set(), self.threshold, window=seconds)
        return r.get("offenders", [])

    def precheck(self, wait=0):
        """Before the game starts: says so if another process is busy on the GPU,
        and waits up to `wait` seconds for it to stop."""
        if not self.counters:
            return
        try:
            self.read()  # the previous read may be from long ago
            busy = self.busy()
            if busy and wait > 0:
                print(f"GPU busy: {describe(busy, 2)}; waiting up to {wait:.0f} s for it to stop")
                start = time.monotonic()
                while busy and time.monotonic() - start < wait:
                    busy = self.busy()
                self.waited = time.monotonic() - start
                if not busy:
                    print(f"GPU free after {self.waited:.0f} s")
            if busy:
                print(f"GPU busy before the run: {describe(busy, 2)}; running anyway "
                      "(the run is marked)")
            self.busy_at_start = busy
        except OSError as e:
            self.errors.append(str(e))

    def start(self, game_pid):
        self.game_pid = game_pid
        if not self.counters:
            return
        try:
            self.read()  # the run's first sample starts now
        except OSError as e:
            self.errors.append(str(e))
        self.thread = threading.Thread(target=self.loop, name="gpu-guard", daemon=True)
        self.thread.start()

    def loop(self):
        while not self.stopping.wait(GPU_INTERVAL):
            t = time.perf_counter()
            try:
                self.samples.append(self.read())
            except OSError as e:
                if len(self.errors) < 10:
                    self.errors.append(str(e))
            except Exception as e:  # never let the guard end a run
                self.errors.append(repr(e))
                break
            self.read_seconds += time.perf_counter() - t

    def finish(self, out=None):
        """Stops sampling; the report (also written to out\\gpu-guard.json)."""
        if self.thread:
            self.stopping.set()
            self.thread.join()
        if not self.counters:
            r = {"clean": None, "error": "GPU Engine counters not available"}
        else:
            names = {pid: info[1] for pid, info in self.procs.items()}
            parents = {pid: info[0] for pid, info in self.procs.items()}
            r = gpu_assess(self.samples, names, game_pids(self.game_pid, parents, self.before),
                           self.threshold)
            if r["clean"] is None and self.errors:
                r["error"] = self.errors[0]
            r["busy_at_start"] = self.busy_at_start
            r["waited_s"] = round(self.waited, 1)
            r["read_ms_mean"] = round(self.read_seconds * 1000 / max(1, len(self.samples)), 2)
            if self.errors:
                r["errors"] = self.errors
        if out is not None:
            (Path(out) / "gpu-guard.json").write_text(json.dumps(r, indent=1), encoding="utf-8")
        return r


def gpu_watch(seconds, threshold):
    """devrun.py gpu: every process's GPU use over the next `seconds`."""
    guard = GpuGuard(threshold)
    if not guard.counters:
        return
    guard.start(None)
    time.sleep(seconds)
    r = guard.finish()
    print(f"{r['samples']} samples over {r['seconds']} s; {GPU_ENGINES} engines; "
          f"{r['read_ms_mean']} ms a read")
    for p in sorted(r.get("processes", []), key=lambda p: -p["mean"]):
        flag = " <- above the threshold" if p in r.get("offenders", []) else ""
        print(f"{p['name']:>28} {p['pid']:7d} {p['role']:>7}: mean {p['mean']:6.2f}%  "
              f"{GPU_WINDOW:g} s {p['window']:6.2f}%  peak {p['peak']:6.2f}%{flag}")
    print(gpu_summary(r))


def fg_cap(a):
    """mcla_fps_frame_generation: --fps-fg, or the same as --fps ("half" is -1)."""
    cap = getattr(a, "fps_fg", None) or a.fps
    return -1 if cap == "half" else cap


def game_args(a, out):
    args = [
        f"--user_data_root={WORK}",
        *([f"--game_data_root={GAME_DATA}"] if GAME_DATA else []),
        f"--cache_root={Path(a.cache_root).resolve() if a.cache_root else CACHE}",
        f"--log_file={out / 'game.log'}",
        f"--mcla_gpu_backend={a.backend}",
        f"--resolution_scale={a.res}",
        f"--mcla_fps={a.fps}",
        # The cap with frame generation on: the same as --fps (so a run's cap doesn't
        # depend on it), or "half" for HALF DISPLAY (-1).
        f"--mcla_fps_frame_generation={fg_cap(a)}",
        # As the launcher passes it with every frame rate; "unset" leaves it to
        # mcla.toml and the game (src/frame_timing.cpp, ConfigureFrameTiming).
        *([] if a.vsync == "unset" else [f"--vsync={a.vsync}"]),
        f"--fullscreen={'true' if a.fullscreen else 'false'}",
        f"--mcla_intro={a.intro}",
        f"--mcla_pad_frames_csv={out / 'frames.csv'}",
        # A wait_until or press_until that times out leaves a picture of where the game was.
        f"--mcla_pad_timeout_shot={out / 'timeout.png'}",
    ]
    if getattr(a, "present_log", False):
        args.append(f"--present_log={out / 'present.csv'}")
    if a.script:
        args.append(f"--mcla_pad_script={Path(a.script).resolve()}")
        if not a.keep_open:
            args.append("--mcla_pad_exit_on_end=true")
    if a.pipe:
        args.append(f"--mcla_pad_pipe={a.pipe}")
    if a.fixed_dt:
        args.append(f"--mcla_fixed_dt={a.fixed_dt}")
    if a.record:
        args.append(f"--mcla_pad_record={Path(a.record).resolve()}")
    elif getattr(a, "track", False):
        # The car's position twice a second (at 60), to compare runs' paths.
        args.append(f"--mcla_pad_record={out / 'pad.csv'}")
    args += a.extra
    # One value per option, the last one: the SDK's command line parser (CLI11) fails on a
    # repeated option, and the SDK then applies none of them, so the game would run on the
    # real Documents\\mcla, its cache and the default API (it happened on 2026-10-17).
    last = {}
    for arg in args:
        key = arg.split("=", 1)[0]
        if key in last:
            print(f"devrun: {key} given twice; the last value is used", flush=True)
            del last[key]
        last[key] = arg
    return list(last.values())


def mcla_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq mcla.exe", "/NH"],
                         capture_output=True, text=True).stdout
    return any(line.lower().startswith("mcla.exe") for line in out.splitlines())


def wait_for_no_game(limit_s):
    start = time.monotonic()
    while mcla_running():
        if time.monotonic() - start > limit_s:
            raise SystemExit(f"another mcla.exe is still running after {limit_s:.0f} s")
        print("another mcla.exe is running; waiting 20 s", flush=True)
        time.sleep(20)


def run(a, tag=None):
    if not EXE.exists():
        raise SystemExit(f"{EXE} is not built")
    # Another game (the user's, or another checkout's run) would share the GPU and
    # the screen: wait for it to close, never touch it.
    wait_for_no_game(getattr(a, "game_wait", 7200))
    prepare()
    restore_work()
    guard = GpuGuard(a.gpu_threshold) if a.gpu_guard else None
    if guard:
        guard.precheck(a.gpu_wait)
    out = RUNS / (tag or time.strftime("%Y%m%d-%H%M%S"))
    out.mkdir(parents=True, exist_ok=True)
    (out / "gpu-guard.json").unlink(missing_ok=True)
    before = fingerprint()
    toml_backup = TOML.read_bytes() if TOML.exists() else None
    env = dict(os.environ, REX_PERF_LOG_CSV=str(out / "perf.csv"))
    # The private folders and the API again as environment variables, which the SDK applies
    # below the command line: if it can't parse the command line, a run still can't reach
    # the real data.
    env["REX_USER_DATA_ROOT"] = str(WORK)
    env["REX_CACHE_ROOT"] = str(Path(a.cache_root).resolve() if a.cache_root else CACHE)
    env["REX_MCLA_GPU_BACKEND"] = a.backend
    if GAME_DATA:
        env["REX_GAME_DATA_ROOT"] = str(GAME_DATA)
    env.update(kv.split("=", 1) for kv in a.env)
    args = game_args(a, out)
    (out / "args.txt").write_text(" ".join(args), encoding="utf-8")
    presentmon = start_presentmon(out, getattr(a, "pm_pcl", False)) if getattr(a, "presentmon", False) else None
    start = time.monotonic()
    proc = None
    restarts = []
    gpu = None
    # Whatever happens from here (Ctrl+C, an exception), the games of this run
    # are closed and the real mcla.toml is put back before anything else.
    try:
        while True:
            # Again just before starting, and before the run's settings file
            # goes in: a game that starts in between would read it, and its
            # own saves would be lost with the restore.
            wait_for_no_game(getattr(a, "game_wait", 7200))
            if a.toml:
                # The settings file the run starts with, instead of the real
                # one; "none" for no file.
                if a.toml == "none":
                    TOML.unlink(missing_ok=True)
                else:
                    shutil.copyfile(a.toml, TOML)
            if not a.toml or not mcla_running():
                break
            print("a game started while the run's mcla.toml went in; putting the real one back",
                  flush=True)
            if toml_backup is not None:
                TOML.write_bytes(toml_backup)
            else:
                TOML.unlink(missing_ok=True)
        stdout_file = open(a.stdout, "wb") if getattr(a, "stdout", None) else None
        proc = subprocess.Popen([str(EXE), *args], cwd=EXE.parent, env=env, stdout=stdout_file)
        if guard:
            guard.start(proc.pid)
        try:
            proc.wait(timeout=a.timeout)
        except subprocess.TimeoutExpired:
            print(f"timeout after {a.timeout} s; closing the game")
            close_windows(proc.pid)
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        restarts = follow_restarts(proc.pid, start + a.timeout)
        if stdout_file:
            stdout_file.close()
    finally:
        if proc is not None:
            end_games(proc)
        if presentmon is not None:
            stop_presentmon(presentmon)
        if guard:
            try:
                gpu = guard.finish(out)
            except Exception as e:  # the restore below must happen regardless
                print(f"GPU guard: {e}")
        try:
            # What the game saved (the pause menu's settings).
            if TOML.exists():
                shutil.copyfile(TOML, out / "mcla.toml.after")
        finally:
            if toml_backup is not None:
                TOML.write_bytes(toml_backup)
            elif TOML.exists():
                TOML.unlink()
    elapsed = time.monotonic() - start
    after = fingerprint()
    if before != after:
        changed = [k for k in set(before) | set(after) if before.get(k) != after.get(k)]
        raise SystemExit(f"REAL DATA CHANGED during the run: {changed}")
    restarted = f", restarted {len(restarts)} time(s)" if restarts else ""
    print(f"{out.name}: exit {proc.returncode} after {elapsed:.0f} s{restarted}; real save data unchanged")
    print(f"{out.name}: {gpu_summary(gpu)}")
    # A script line that failed ends the script (and the game): the run is not
    # what the script describes.
    log = out / "game.log"
    if log.exists():
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            if "dev pad: script " in line and " done: err" in line:
                shot = f" (screenshot: {out / 'timeout.png'})" if (out / "timeout.png").exists() else ""
                raise SystemExit(f"{out.name}: SCRIPT FAILED: {line.split(' done: ', 1)[1]}{shot}")
    return out


def start_presentmon(out, pc_latency=False):
    """PresentMon capturing the game's presents for the run, into presentmon.csv (QPC times, so
    presents.py can line them up with the swaps and the present log)."""
    if not PRESENTMON or not Path(PRESENTMON).exists():
        raise SystemExit("--presentmon: no PresentMon (winget install Intel.PresentMon.Console, or "
                         "set MCLA_PRESENTMON)")
    log = open(out / "presentmon.log", "w", encoding="utf-8")
    proc = subprocess.Popen([str(PRESENTMON), "--process_name", EXE.name, "--output_file",
                             str(out / "presentmon.csv"), "--v2_metrics", "--qpc_time",
                             "--terminate_on_proc_exit", "--no_console_stats",
                             "--stop_existing_session", "--session_name", "mcla-devrun",
                             *(["--track_pc_latency"] if pc_latency else [])],
                            stdout=log, stderr=subprocess.STDOUT)
    # Its trace session must be up before the game's first present.
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline and proc.poll() is None:
        if "Started recording" in (out / "presentmon.log").read_text(encoding="utf-8", errors="replace"):
            break
        time.sleep(0.2)
    if proc.poll() is not None:
        raise SystemExit(f"PresentMon exited: {(out / 'presentmon.log').read_text(errors='replace')[-500:]}")
    return proc


def stop_presentmon(proc):
    try:
        proc.wait(timeout=20)  # --terminate_on_proc_exit
    except subprocess.TimeoutExpired:
        subprocess.run([str(PRESENTMON), "--terminate_existing_session", "--session_name", "mcla-devrun"],
                       capture_output=True)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


def load_segments(path):
    path = Path(path)
    if path.is_dir():
        path = path / "frames.csv"
    segments = [("start", [], 0)]
    for line in path.read_text(encoding="utf-8").splitlines()[1:]:
        kind, *rest = line.split(",")
        if len(rest) < 3 or not all(field.strip("-").isdigit() for field in rest[1:3]):
            continue  # the last line of a game that was killed mid-write
        if kind == "mark":
            segments.append((rest[0], [], int(rest[2])))
        elif kind == "s" and int(rest[1]) > 0:
            segments[-1][1].append(int(rest[1]) / 1000.0)
    return segments


def stats(frame_ms):
    if not frame_ms:
        return None
    ordered = sorted(frame_ms)
    worst = ordered[-max(1, len(ordered) // 100):]
    return {
        "frames": len(frame_ms),
        "seconds": sum(frame_ms) / 1000.0,
        "median_ms": statistics.median(frame_ms),
        "mean_fps": 1000.0 * len(frame_ms) / sum(frame_ms),
        "p99_ms": ordered[min(len(ordered) - 1, int(len(ordered) * 0.99))],
        "low1_fps": 1000.0 / statistics.mean(worst),
    }


def analyze(path, quiet=False, gpu=False):
    result = {}
    for name, frames, _ in load_segments(path):
        s = stats(frames)
        if s:
            result[name] = s
            if not quiet:
                print(f"{name:>16}: {s['frames']:6d} swaps {s['seconds']:7.1f} s  median "
                      f"{s['median_ms']:6.2f} ms  mean {s['mean_fps']:6.1f} fps  p99 "
                      f"{s['p99_ms']:6.2f} ms  1% low {s['low1_fps']:6.1f} fps")
    if gpu:
        folder = Path(path) if Path(path).is_dir() else Path(path).parent
        r = gpu_report(folder)
        print(gpu_summary(r) if r else "GPU guard: no gpu-guard.json in this run")
    return result


def track(out, marks=("drive", "race")):
    """Positions after the first of these marks in a run's pad.csv:
    {frames since the mark: (x, y, z)}."""
    path = Path(out) / "pad.csv"
    if not path.exists():
        return {}
    start, points = None, {}
    for line in path.read_text(encoding="utf-8").splitlines():
        f = line.split(",")
        if f[0] == "mark" and f[2] in marks and start is None:
            start = int(f[1])
        elif f[0] == "pos" and start is not None:
            points[int(f[1]) - start] = tuple(map(float, f[2:5]))
    return points


def watch(out):
    """The "watch" lines in a run's log: {mode timer tick: [(x, y, z) per car]},
    the player first, then the opponents in the order they were made."""
    log = Path(out) / "game.log"
    result = {}
    if not log.exists():
        return result
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if "dev pad: watch tick=" not in line:
            continue
        head, *parts = line.split("|")
        tick = int(head.split("tick=")[1].split()[0])
        result[tick] = [tuple(map(float, p.split())) for p in parts]
    return result


def watch_report(run, reference, fps=60):
    """Where a run's cars first left the reference run's (more than 1 cm; car 0
    is the player) and the farthest any car was from it at a few points."""
    common = sorted(t for t in set(run) & set(reference) if len(run[t]) == len(reference[t]))
    if not common:
        return "no common watch points"
    first = None
    for t in common:
        devs = [math.dist(p, q) for p, q in zip(run[t], reference[t])]
        if max(devs) > 0.01 and first is None:
            car = devs.index(max(devs))
            first = f"first apart at {t / fps:.0f} s (car {car}, {max(devs):.2f} m)"
    points = [common[min(len(common) - 1, i * (len(common) - 1) // 4)] for i in range(1, 5)]
    farthest = ", ".join(
        f"{t / fps:.0f} s {max(math.dist(p, q) for p, q in zip(run[t], reference[t])):.1f} m"
        for t in points)
    return f"{first or 'identical'}; farthest car at {farthest}"


def deviation(points, reference):
    common = sorted(set(points) & set(reference))
    if not common:
        return None
    return max(sum((p - q) ** 2 for p, q in zip(points[k], reference[k])) ** 0.5 for k in common)


def bench(a):
    configs = [c.split(":") for c in a.configs.split(",")]
    results = {}
    reference = None
    reference_watch = None
    redone, left_out, unchecked = [], [], 0
    retries = a.gpu_retries
    for i in range(a.runs + a.warmup):
        for backend, res in configs:
            a.backend, a.res = backend, res
            slot = tag = f"{a.name}-{backend}-x{res}-{i}"
            attempt = 0
            # A run another process shared the GPU with is redone at once, so
            # the configurations still alternate; warm-up runs are not redone.
            while True:
                out = run(a, tag)
                gpu = gpu_report(out)
                contaminated = bool(gpu) and gpu.get("clean") is False
                if not contaminated or i < a.warmup:
                    break
                who = describe(gpu["offenders"], gpu["window_s"])
                if retries <= 0:
                    left_out.append(f"{tag}: {who}")
                    print(f"{tag}: contaminated and the redo cap is reached; left out")
                    break
                retries -= 1
                attempt += 1
                redone.append(f"{tag}: {who}")
                tag = f"{slot}-redo{attempt}"
                print(f"{tag}: redoing {slot}, which another process shared the GPU with")
            if i < a.warmup:
                continue
            if contaminated:
                continue
            if not gpu or gpu.get("clean") is None:
                unchecked += 1
            for seg, s in analyze(out, quiet=True).items():
                results.setdefault((backend, res, seg), []).append(s)
            points = track(out)
            if points:
                if reference is None:
                    reference = points
                dev = deviation(points, reference)
                shown = "no common positions" if dev is None else f"max {dev:.2f} m"
                print(f"{tag}: path of {len(points)} positions, {shown} from the first run's")
            watched = watch(out)
            if watched:
                if reference_watch is None:
                    reference_watch = watched
                print(f"{tag}: {len(next(iter(watched.values())))} cars against the first run: "
                      f"{watch_report(watched, reference_watch, a.fixed_dt or 60)}")
    print(f"\n{a.runs} runs per configuration (after {a.warmup} warm-up), alternating:")
    for (backend, res, seg), runs in sorted(results.items(), key=lambda kv: (kv[0][2], kv[0][0], kv[0][1])):
        med = [r["median_ms"] for r in runs]
        low = [r["low1_fps"] for r in runs]
        fps = [r["mean_fps"] for r in runs]
        secs = [r["seconds"] for r in runs]
        spread = (max(med) - min(med)) / statistics.median(med) * 100 if len(med) > 1 else 0
        print(f"{seg:>16} {backend:>6} x{res}: median {statistics.median(med):6.2f} ms "
              f"(runs {', '.join(f'{m:.2f}' for m in med)}; spread {spread:4.1f}%)  mean "
              f"{statistics.median(fps):6.1f} fps  1% low {statistics.median(low):6.1f} fps  "
              f"length {statistics.median(secs):5.1f} s ({min(secs):.1f}-{max(secs):.1f})")
    for backend, res in configs:
        if not any(k[:2] == (backend, res) for k in results):
            print(f"{'':>16} {backend:>6} x{res}: no clean runs")
    if redone:
        print(f"GPU guard: {len(redone)} run(s) redone because another process used the GPU:")
        for line in redone:
            print(f"  {line}")
    if left_out:
        print(f"NOT RELIABLE: other processes kept using the GPU, the redo cap (--gpu-retries "
              f"{a.gpu_retries}) was reached and {len(left_out)} contaminated run(s) are left "
              "out of the statistics above, so the configurations are not compared on the same "
              "number of runs:")
        for line in left_out:
            print(f"  {line}")
    elif not a.gpu_guard:
        print("GPU guard: off; the runs were not checked for other processes using the GPU")
    elif unchecked:
        print(f"GPU guard: {unchecked} run(s) not checked (GPU Engine counters not available)")
    else:
        print("GPU guard: every run counted was clean")


def add_gpu_options(p, wait=0):
    p.add_argument("--no-gpu-guard", dest="gpu_guard", action="store_false",
                   help="don't check for other processes using the GPU")
    p.add_argument("--gpu-threshold", type=float, default=GPU_THRESHOLD, metavar="PERCENT",
                   help="another process's GPU use (3D and compute engines, %% of one engine) "
                   f"over the run or any {GPU_WINDOW:g} s that marks the run as contaminated "
                   "(default %(default)s)")
    p.add_argument("--game-wait", type=float, default=7200, metavar="SECONDS",
                   help="while another mcla.exe runs, wait up to this long (polling every 20 s) "
                   "before starting (default %(default)s)")
    p.add_argument("--gpu-wait", type=float, default=wait, metavar="SECONDS",
                   help="if another process is busy on the GPU before a run, wait up to this "
                   "long for it to stop, then run anyway (default %(default)s)")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--refresh", action="store_true", help="snapshot the real save again")
    for name in ("run", "bench"):
        p = sub.add_parser(name)
        p.add_argument("--script")
        p.add_argument("--pipe", default="", help="also serve commands on this pipe name")
        p.add_argument("--backend", default="d3d12", choices=["d3d12", "vulkan"])
        p.add_argument("--res", default="1")
        p.add_argument("--fps", default="0", help="frame cap, 0 = uncapped")
        p.add_argument("--fps-fg", default=None, help="the cap with frame generation on "
                       "(mcla_fps_frame_generation): default the same as --fps; half = HALF DISPLAY")
        p.add_argument("--fixed-dt", type=int, default=0, help="timedemo: updates of exactly 1/N s")
        # "fast" unpaces the start-up logos (src/frame_timing.cpp, PaceUnlimitedSwap);
        # benchmarks keep "normal" so start-up runs as the user sees it.
        p.add_argument("--intro", default="normal" if name == "bench" else "fast")
        p.add_argument("--fullscreen", action="store_true")
        p.add_argument("--vsync", default="false", choices=["false", "true", "unset"],
                       help="--vsync on the command line (default false, as the launcher); "
                       "unset passes none")
        p.add_argument("--toml", help="start the run with this mcla.toml instead of the real "
                       "one (\"none\": with none); the real one is put back afterwards, and "
                       "what the game saved is kept as mcla.toml.after in the run folder")
        p.add_argument("--record", help="record user 0's controller to this CSV")
        p.add_argument("--timeout", type=float, default=600)
        p.add_argument("--stdout", help="the game's standard output to this file (as a front end "
                       "that reads it gets it; otherwise the game, a GUI program, has none)")
        p.add_argument("--keep-open", action="store_true", help="don't quit when the script ends")
        p.add_argument("--track", action="store_true", help="log the car's path (pad.csv)")
        p.add_argument("--cache-root", help="shader storage folder (default: the shared one under "
                       r"%%LOCALAPPDATA%%\mcla-dev\cache), e.g. an empty one for a first run")
        p.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                       help="environment variable for the game (repeatable)")
        p.add_argument("--presentmon", action="store_true",
                       help="capture the game's presents with PresentMon (presentmon.csv)")
        p.add_argument("--pm-pcl", action="store_true",
                       help="with --presentmon: also track the PC latency (the Reflex and "
                       "Anti-Lag markers' PCLStats events)")
        p.add_argument("--present-log", action="store_true",
                       help="the SDK's log of refreshes and presents (present.csv)")
        p.add_argument("--extra", nargs=argparse.REMAINDER, default=[])
        # A bench waits for another GPU user to stop, also before a redo.
        add_gpu_options(p, wait=120 if name == "bench" else 0)
        if name == "bench":
            p.add_argument("--configs", default="d3d12:1,vulkan:1")
            p.add_argument("--runs", type=int, default=3)
            p.add_argument("--warmup", type=int, default=1)
            p.add_argument("--name", default="bench")
            p.add_argument("--gpu-retries", type=int, default=3, metavar="N",
                           help="redo contaminated runs up to N times in all (default %(default)s)")
    p = sub.add_parser("record")
    p.add_argument("file")
    p.add_argument("--backend", default="d3d12", choices=["d3d12", "vulkan"])
    p.add_argument("--res", default="1")
    p.add_argument("--fps", default="120")
    p.add_argument("--fullscreen", action="store_true")
    add_gpu_options(p)
    p = sub.add_parser("analyze")
    p.add_argument("path")
    p = sub.add_parser("gpu", help="which processes use the GPU right now")
    p.add_argument("--seconds", type=int, default=10)
    p.add_argument("--gpu-threshold", type=float, default=GPU_THRESHOLD, metavar="PERCENT")
    a = parser.parse_args()
    if a.cmd == "prepare":
        prepare(a.refresh)
    elif a.cmd == "run":
        out = run(a)
        analyze(out)
        if (out / "present.csv").exists() or (out / "presentmon.csv").exists():
            sys.path.insert(0, str(HERE))  # python -I leaves the script folder out
            import presents
            presents.report(out)
    elif a.cmd == "bench":
        bench(a)
    elif a.cmd == "record":
        # Same settings for the playback: --fixed-dt is off, so play it back at
        # the cap it was recorded at.
        a.script, a.pipe, a.fixed_dt, a.intro = str(HERE / "scripts" / "mark-roam.pad"), "", 0, "normal"
        a.record, a.keep_open, a.timeout, a.extra = a.file, True, 3 * 3600, []
        a.cache_root, a.env, a.vsync, a.toml = None, [], "false", None
        analyze(run(a))
    elif a.cmd == "gpu":
        gpu_watch(a.seconds, a.gpu_threshold)
    else:
        analyze(a.path, gpu=True)


if __name__ == "__main__":
    main()

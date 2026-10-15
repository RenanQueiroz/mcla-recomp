"""Unattended mcla.exe runs with scripted controller input (fork-only dev tool).

Every run plays on a scratch copy of the save data, never on the real one:

  devrun.py prepare            copy Documents\\mcla (save, achievements) to a
                               pristine snapshot under %LOCALAPPDATA%\\mcla-dev
  devrun.py run --script scripts\\boot-to-roam.pad [--backend vulkan] [--res 3]
  devrun.py bench --script scripts\\boot-to-roam.pad --configs d3d12:1,vulkan:1 --runs 3
  devrun.py analyze <run folder or frames.csv>
  devrun.py record <drive.csv> [--fps 120] [--fullscreen]
                               play on the save copy with your own controller;
                               your inputs are recorded from the moment you are
                               in the city (close the game to stop)

Before each run the scratch copy is restored from the snapshot and mcla.toml
(next to mcla.exe, where the game saves its settings) is backed up; after the
run mcla.toml is put back, and the real save data and mcla.toml are checked
to be byte-identical to before the run.
"""

import argparse
import ctypes
import hashlib
import os
import shutil
import statistics
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
EXE = REPO / "out" / "build" / "win-amd64-release" / "mcla.exe"
TOML = EXE.with_name("mcla.toml")
REAL_USER = Path(os.environ["USERPROFILE"]) / "Documents" / "mcla"
DEV = Path(os.environ["LOCALAPPDATA"]) / "mcla-dev"
PRISTINE = DEV / "pristine"
WORK = DEV / "work"
CACHE = DEV / "cache"
RUNS = DEV / "runs"

# The launcher's always-on settings (launcher/src/Backend.cs, Tuning).
TUNING = [
    "--clear_memory_page_state=false",
    "--execute_unclipped_draw_vs_on_cpu=true",
    "--shared_memory_hot_page_faults=4",
    "--shared_memory_hot_page_ms=10000",
]


def real_files():
    files = [p for p in REAL_USER.rglob("*") if p.is_file() and "cache" not in p.relative_to(REAL_USER).parts]
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


def game_args(a, out):
    args = [
        f"--user_data_root={WORK}",
        f"--cache_root={CACHE}",
        f"--log_file={out / 'game.log'}",
        f"--mcla_gpu_backend={a.backend}",
        f"--resolution_scale={a.res}",
        f"--mcla_fps={a.fps}",
        "--vsync=false",
        f"--fullscreen={'true' if a.fullscreen else 'false'}",
        f"--mcla_intro={a.intro}",
        f"--mcla_pad_frames_csv={out / 'frames.csv'}",
        *TUNING,
    ]
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
    args += a.extra
    return args


def run(a, tag=None):
    if not EXE.exists():
        raise SystemExit(f"{EXE} is not built")
    for p in subprocess.run(["tasklist", "/FI", "IMAGENAME eq mcla.exe", "/NH"],
                            capture_output=True, text=True).stdout.splitlines():
        if p.lower().startswith("mcla.exe"):
            raise SystemExit("mcla.exe is already running")
    prepare()
    restore_work()
    out = RUNS / (tag or time.strftime("%Y%m%d-%H%M%S"))
    out.mkdir(parents=True, exist_ok=True)
    before = fingerprint()
    toml_backup = TOML.read_bytes() if TOML.exists() else None
    env = dict(os.environ, REX_PERF_LOG_CSV=str(out / "perf.csv"))
    args = game_args(a, out)
    (out / "args.txt").write_text(" ".join(args), encoding="utf-8")
    start = time.monotonic()
    proc = subprocess.Popen([str(EXE), *args], cwd=EXE.parent, env=env)
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
    elapsed = time.monotonic() - start
    if toml_backup is not None:
        TOML.write_bytes(toml_backup)
    elif TOML.exists():
        TOML.unlink()
    after = fingerprint()
    if before != after:
        changed = [k for k in set(before) | set(after) if before.get(k) != after.get(k)]
        raise SystemExit(f"REAL DATA CHANGED during the run: {changed}")
    print(f"{out.name}: exit {proc.returncode} after {elapsed:.0f} s; real save data unchanged")
    return out


def load_segments(path):
    path = Path(path)
    if path.is_dir():
        path = path / "frames.csv"
    segments = [("start", [], 0)]
    for line in path.read_text(encoding="utf-8").splitlines()[1:]:
        kind, *rest = line.split(",")
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


def analyze(path, quiet=False):
    result = {}
    for name, frames, _ in load_segments(path):
        s = stats(frames)
        if s:
            result[name] = s
            if not quiet:
                print(f"{name:>16}: {s['frames']:6d} swaps {s['seconds']:7.1f} s  median "
                      f"{s['median_ms']:6.2f} ms  mean {s['mean_fps']:6.1f} fps  p99 "
                      f"{s['p99_ms']:6.2f} ms  1% low {s['low1_fps']:6.1f} fps")
    return result


def bench(a):
    configs = [c.split(":") for c in a.configs.split(",")]
    results = {}
    for i in range(a.runs + a.warmup):
        for backend, res in configs:
            a.backend, a.res = backend, res
            tag = f"{a.name}-{backend}-x{res}-{i}"
            out = run(a, tag)
            if i < a.warmup:
                continue
            for seg, s in analyze(out, quiet=True).items():
                results.setdefault((backend, res, seg), []).append(s)
    print(f"\n{a.runs} runs per configuration (after {a.warmup} warm-up), alternating:")
    for (backend, res, seg), runs in sorted(results.items(), key=lambda kv: (kv[0][2], kv[0][0], kv[0][1])):
        med = [r["median_ms"] for r in runs]
        low = [r["low1_fps"] for r in runs]
        fps = [r["mean_fps"] for r in runs]
        spread = (max(med) - min(med)) / statistics.median(med) * 100 if len(med) > 1 else 0
        print(f"{seg:>16} {backend:>6} x{res}: median {statistics.median(med):6.2f} ms "
              f"(runs {', '.join(f'{m:.2f}' for m in med)}; spread {spread:4.1f}%)  mean "
              f"{statistics.median(fps):6.1f} fps  1% low {statistics.median(low):6.1f} fps")


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
        p.add_argument("--fixed-dt", type=int, default=0, help="timedemo: updates of exactly 1/N s")
        # "fast" also unpaces untimed loops (src/frame_timing.cpp, PaceUnlimitedSwap),
        # so benchmarks run the game as the user plays it.
        p.add_argument("--intro", default="normal" if name == "bench" else "fast")
        p.add_argument("--fullscreen", action="store_true")
        p.add_argument("--record", help="record user 0's controller to this CSV")
        p.add_argument("--timeout", type=float, default=600)
        p.add_argument("--keep-open", action="store_true", help="don't quit when the script ends")
        p.add_argument("--extra", nargs=argparse.REMAINDER, default=[])
        if name == "bench":
            p.add_argument("--configs", default="d3d12:1,vulkan:1")
            p.add_argument("--runs", type=int, default=3)
            p.add_argument("--warmup", type=int, default=1)
            p.add_argument("--name", default="bench")
    p = sub.add_parser("record")
    p.add_argument("file")
    p.add_argument("--backend", default="d3d12", choices=["d3d12", "vulkan"])
    p.add_argument("--res", default="1")
    p.add_argument("--fps", default="120")
    p.add_argument("--fullscreen", action="store_true")
    p = sub.add_parser("analyze")
    p.add_argument("path")
    a = parser.parse_args()
    if a.cmd == "prepare":
        prepare(a.refresh)
    elif a.cmd == "run":
        analyze(run(a))
    elif a.cmd == "bench":
        bench(a)
    elif a.cmd == "record":
        # Same settings for the playback: --fixed-dt is off, so play it back at
        # the cap it was recorded at.
        a.script, a.pipe, a.fixed_dt, a.intro = str(HERE / "scripts" / "mark-roam.pad"), "", 0, "normal"
        a.record, a.keep_open, a.timeout, a.extra = a.file, True, 3 * 3600, []
        analyze(run(a))
    else:
        analyze(a.path)


if __name__ == "__main__":
    main()

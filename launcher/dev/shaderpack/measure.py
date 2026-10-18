"""Measurements for shader preparation (fork-only development tool).

  python measure.py prepare --backend d3d12 --res 3 --salt 101 [--twice]
      a preparation run (mcla.exe --mcla_prepare_shaders) on a new, empty storage; with --twice a
      second one on the same storage (driver cache warm for its pipelines)
  python measure.py race --backend vulkan --res 3 --salt 102 [--prepared]
      the race benchmark (race.pad, timedemo) on a new storage; with --prepared, the storage is
      prepared first with the same salt, as a player's would be after the launcher's step;
      without it the import at start is off too (--mcla_shader_pack_auto=false)

--salt: the dev patch's tmp_shader_salt (launcher/dev/shader-salt.sdk.patch), which makes the
translated shaders new to the driver's cache, as after a driver update: use a new value per
measurement. Runs go through devrun.py (private save copy, private storage under MCLA_DEV_ROOT,
real data checked), so set MCLA_DEV_ROOT and, for a checkout without the game, MCLA_GAME_DATA.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEVRUN = HERE.parent / "devrun.py"
sys.path.insert(0, str(HERE.parent))
import devrun  # noqa: E402  (load_segments, stats)


def run_devrun(args, tag):
    cmd = [sys.executable, str(DEVRUN), "run", *args]
    print("devrun", " ".join(args), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    out = proc.stdout + proc.stderr
    (Path(os.environ.get("MCLA_DEV_ROOT", ".")) / f"measure-{tag}.log").write_text(out, encoding="utf-8")
    if "REAL DATA CHANGED" in out:
        raise SystemExit("REAL DATA CHANGED")
    m = re.search(r"^(\d{8}-\d{6}\S*): exit (-?\d+)", out, re.M)
    folder = devrun.RUNS / m.group(1) if m else None
    return out, folder


def prepare(backend, res, salt, cache, tag):
    # The game's standard output, as the launcher reads it (ShaderPrep.cs).
    stdout = Path(os.environ["MCLA_DEV_ROOT"]) / f"measure-{tag}.stdout"
    _, folder = run_devrun(["--backend", backend, "--res", str(res), "--cache-root", str(cache),
                            "--timeout", "1800", "--no-gpu-guard", "--stdout", str(stdout),
                            "--extra", "--mcla_prepare_shaders=true", f"--tmp_shader_salt={salt}"], tag)
    out = stdout.read_text(encoding="utf-8", errors="replace") if stdout.exists() else ""
    r = {"tag": tag, "run": folder.name if folder else None,
         "progress_lines": sum(1 for line in out.splitlines() if line.startswith("SHADERPREP preload "))}
    for line in out.splitlines():
        if line.startswith("SHADERPREP done "):
            fields = line.split()[2:]
            rb, pl, priv, gpu = fields[:4]
            r.update(rebuild_ms=int(rb), preload_ms=int(pl), private_mb=float(priv), gpu_mb=float(gpu))
            if len(fields) > 4:
                r["process_ms"] = int(fields[4])  # from the process's creation to the report
        elif line.startswith("SHADERPREP preload-done "):
            created, total, ms = line.split()[2:5]
            r.update(created=int(created), total=int(total), create_ms=int(ms))
        elif line.startswith("SHADERPREP rebuild-done "):
            ok, failed, ms = line.split()[2:5]
            r.update(rebuilt=int(ok), rebuild_failed=int(failed))
        elif line.startswith("SHADERPREP merge "):
            sh, pl = line.split()[2:4]
            r.update(shaders_added=int(sh), pipelines_added=int(pl))
        elif line.startswith("SHADERPREP error "):
            r["error"] = line
    return r


def race(backend, res, salt, cache, tag, auto_import):
    # Without the pack: its import at start off too (it is on by default).
    out, folder = run_devrun(["--script", str(HERE.parent / "scripts" / "race.pad"), "--backend", backend,
                              "--res", str(res), "--fixed-dt", "60", "--cache-root", str(cache),
                              "--timeout", "900", "--extra", f"--tmp_shader_salt={salt}",
                              f"--mcla_shader_pack_auto={'true' if auto_import else 'false'}"], tag)
    return race_result(folder, tag, cache)


# Pipelines in a storage folder (the file of the API the run used): those the run created during
# play are what the storage holds after it minus what it preloaded.
STORAGE_RECORD = {"rtv.d3d12.xpso": 72, "rov.d3d12.xpso": 72, "fbo.vk.xpso": 68, "fsi.vk.xpso": 68}


def stored_pipelines(cache):
    counts = {}
    for name, size in STORAGE_RECORD.items():
        f = Path(cache) / "shaders" / "shareable" / f"545407F8.{name}"
        if f.exists():
            counts[name] = (f.stat().st_size - 12) // size
    return counts


def race_result(folder, tag, cache=None):
    segs = devrun.load_segments(folder / "frames.csv")
    after_title, seen = [], False
    for name, frames, _ in segs:
        seen |= name == "title"
        if seen:
            after_title += frames
    r = {"tag": tag, "run": folder.name,
         "over50": sum(1 for f in after_title if f > 50), "over100": sum(1 for f in after_title if f > 100),
         "worst_ms": round(max(after_title), 1), "time_over50_s": round(sum(f for f in after_title if f > 50) / 1000, 2)}
    for name, frames, _ in segs:
        s = devrun.stats(frames)
        if s and name in ("map_zoom", "race"):
            r[f"{name}_low1_fps"] = round(s["low1_fps"], 1)
            r[f"{name}_median_ms"] = round(s["median_ms"], 2)
    log = (folder / "game.log").read_text(encoding="utf-8", errors="replace")
    m = re.search(r"Created (\d+) graphics pipelines", log)
    r["preloaded_at_start"] = int(m.group(1)) if m else 0
    if cache:
        used = "rtv.d3d12.xpso" if "d3d12" in tag else "fbo.vk.xpso"
        r["stored_after"] = stored_pipelines(cache).get(used, 0)
        r["created_in_play"] = r["stored_after"] - r["preloaded_at_start"]
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["prepare", "race"])
    ap.add_argument("--backend", default="d3d12")
    ap.add_argument("--res", default="3")
    ap.add_argument("--salt", type=int, required=True)
    ap.add_argument("--twice", action="store_true")
    ap.add_argument("--prepared", action="store_true")
    a = ap.parse_args()
    root = Path(os.environ["MCLA_DEV_ROOT"])
    stamp = time.strftime("%H%M%S")
    cache = root / f"cache-{a.what}-{a.backend}-{a.salt}-{stamp}"
    results = []
    if a.what == "prepare":
        results.append(prepare(a.backend, a.res, a.salt, cache, f"prep-{a.backend}-{a.salt}-1"))
        if a.twice:
            results.append(prepare(a.backend, a.res, a.salt, cache, f"prep-{a.backend}-{a.salt}-2"))
    else:
        if a.prepared:
            results.append(prepare(a.backend, a.res, a.salt, cache, f"prep-{a.backend}-{a.salt}"))
        results.append(race(a.backend, a.res, a.salt, cache, f"race-{a.backend}-{a.salt}-{'pack' if a.prepared else 'none'}",
                            auto_import=a.prepared))
    for r in results:
        print(json.dumps(r))
    with open(root / "measure-results.jsonl", "a", encoding="utf-8") as f:
        for r in results:
            f.write(json.dumps(dict(r, backend=a.backend, res=a.res, salt=a.salt)) + "\n")


if __name__ == "__main__":
    main()

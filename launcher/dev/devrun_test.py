"""Tests of devrun.py's GPU guard on synthetic samples (no GPU or game needed):

    python launcher\\dev\\devrun_test.py
"""

import argparse
import contextlib
import io
import unittest
from pathlib import Path
from unittest import mock

import devrun

GAME, CHILD, DWM, OTHER, BROWSER = 100, 101, 200, 300, 400
NAMES = {GAME: "mcla.exe", CHILD: "helper.exe", DWM: "dwm.exe", OTHER: "forza.exe",
         BROWSER: "chrome.exe"}


def samples(n, **series):
    """n one-second samples; series: pid name -> value, or a function of the index
    returning a value or None (absent from that sample, e.g. exited)."""
    pids = {"game": GAME, "child": CHILD, "dwm": DWM, "other": OTHER, "browser": BROWSER}
    out = []
    for i in range(n):
        usage = {}
        for key, v in series.items():
            value = v(i) if callable(v) else v
            if value is not None:
                usage[pids[key]] = value
        out.append((1.0, usage))
    return out


def assess(s, threshold=5.0):
    return devrun.gpu_assess(s, NAMES, {GAME, CHILD}, threshold)


class Assess(unittest.TestCase):
    def test_other_process_over_threshold(self):
        r = assess(samples(60, game=90.0, other=20.0))
        self.assertFalse(r["clean"])
        self.assertEqual([p["pid"] for p in r["offenders"]], [OTHER])
        self.assertAlmostEqual(r["offenders"][0]["mean"], 20.0)
        self.assertAlmostEqual(r["game"]["mean"], 90.0)

    def test_other_process_under_threshold(self):
        r = assess(samples(60, game=90.0, browser=2.0))
        self.assertTrue(r["clean"])
        self.assertEqual(r["busiest_others"][0]["pid"], BROWSER)

    def test_threshold_is_exclusive_and_an_option(self):
        self.assertTrue(assess(samples(30, other=5.0))["clean"])
        self.assertFalse(assess(samples(30, other=5.0), threshold=4.9)["clean"])

    def test_short_spike_is_averaged_over_the_window(self):
        # One second at 20%: 4% over 5 s, under the threshold.
        r = assess(samples(60, game=90.0, browser=lambda i: 20.0 if i == 30 else 0.0))
        self.assertTrue(r["clean"])
        self.assertAlmostEqual(r["busiest_others"][0]["window"], 4.0)
        self.assertAlmostEqual(r["busiest_others"][0]["peak"], 20.0)
        # One second at 40% is 8% over 5 s.
        r = assess(samples(60, game=90.0, browser=lambda i: 40.0 if i == 30 else 0.0))
        self.assertFalse(r["clean"])

    def test_burst_caught_by_window_though_run_mean_is_low(self):
        # 6 s at 30% in a 120 s run: 1.5% mean, 30% over 5 s.
        r = assess(samples(120, game=90.0, other=lambda i: 30.0 if 50 <= i < 56 else 0.0))
        self.assertFalse(r["clean"])
        o = r["offenders"][0]
        self.assertAlmostEqual(o["mean"], 1.5)
        self.assertAlmostEqual(o["window"], 30.0)

    def test_pid_that_exits_mid_run(self):
        # Busy for the first 10 s, then gone: absent samples count as 0.
        r = assess(samples(60, game=90.0, other=lambda i: 30.0 if i < 10 else None))
        self.assertFalse(r["clean"])
        o = r["offenders"][0]
        self.assertAlmostEqual(o["mean"], 5.0)
        self.assertAlmostEqual(o["window"], 30.0)
        # A light one that exits stays clean.
        r = assess(samples(60, game=90.0, browser=lambda i: 3.0 if i < 10 else None))
        self.assertTrue(r["clean"])

    def test_dwm_and_the_game_are_excluded(self):
        r = assess(samples(60, game=99.0, child=40.0, dwm=35.0))
        self.assertTrue(r["clean"])
        roles = {p["pid"]: p["role"] for p in r["processes"]}
        self.assertEqual(roles, {GAME: "game", CHILD: "game", DWM: "ignored"})
        self.assertAlmostEqual(r["game"]["mean"], 139.0)  # the game and its child

    def test_weighted_by_the_time_each_sample_covers(self):
        s = [(3.0, {OTHER: 10.0}), (1.0, {OTHER: 0.0})]
        r = devrun.gpu_assess(s, NAMES, set(), threshold=50.0)
        self.assertAlmostEqual(r["busiest_others"][0]["mean"], 7.5)

    def test_run_shorter_than_the_window(self):
        # 3 s: the window is the whole run; 18% for 1 s of 3 is 6%, 9% is 3%.
        r = assess(samples(3, other=lambda i: [0.0, 0.0, 18.0][i]))
        self.assertFalse(r["clean"])
        self.assertAlmostEqual(r["offenders"][0]["window"], 6.0)
        self.assertTrue(assess(samples(3, other=lambda i: [0.0, 0.0, 9.0][i]))["clean"])

    def test_no_samples(self):
        self.assertIsNone(assess([])["clean"])

    def test_counters_with_no_data_are_not_clean(self):
        r = assess([(1.0, {})] * 10)
        self.assertIsNone(r["clean"])
        self.assertIn("no data", devrun.gpu_summary(r))


class GamePids(unittest.TestCase):
    def test_children_and_grandchildren(self):
        parents = {GAME: 1, CHILD: GAME, 102: CHILD, OTHER: 1}
        self.assertEqual(devrun.game_pids(GAME, parents), {GAME, CHILD, 102})

    def test_process_older_than_the_game_is_not_its_child(self):
        # OTHER's parent died long ago and its pid was given to the game.
        parents = {GAME: 1, OTHER: GAME, CHILD: GAME}
        self.assertEqual(devrun.game_pids(GAME, parents, before={OTHER}), {GAME, CHILD})

    def test_no_game(self):
        self.assertEqual(devrun.game_pids(None, {5: 0}), set())


class InstanceNames(unittest.TestCase):
    def test_key(self):
        c = devrun.GpuCounters.__new__(devrun.GpuCounters)
        c.keys = {}
        base = "pid_1234_luid_0x00000000_0x00011FB2_phys_0_eng_{}_engtype_{}"
        self.assertEqual(c.key(base.format(0, "3D")), (1234, True))
        self.assertEqual(c.key(base.format(5, "Compute_1")), (1234, True))
        self.assertEqual(c.key(base.format(3, "Copy")), (1234, False))
        self.assertEqual(c.key(base.format(1, "VideoDecode")), (1234, False))
        self.assertEqual(c.key("_Total"), (None, False))


class Summary(unittest.TestCase):
    def test_lines(self):
        clean = assess(samples(10, game=80.0, browser=1.0))
        self.assertIn("clean", devrun.gpu_summary(clean))
        self.assertIn("chrome.exe", devrun.gpu_summary(clean))
        dirty = assess(samples(10, game=80.0, other=30.0))
        line = devrun.gpu_summary(dirty)
        self.assertIn("CONTAMINATED", line)
        self.assertIn(f"forza.exe (pid {OTHER}) 30.0% mean", line)
        self.assertIn("not checked", devrun.gpu_summary({"clean": None, "error": "x"}))
        self.assertEqual(devrun.gpu_summary(None), "GPU guard: off")


class Unavailable(unittest.TestCase):
    """Counters that are not there (a real PDH call on a counter set that does not
    exist): one warning, and runs go on unchecked."""

    def test_warns_once_and_continues(self):
        text = io.StringIO()
        with mock.patch.object(devrun, "GPU_COUNTER", r"\No Such GPU Object(*)\Nothing"), \
                mock.patch.object(devrun, "_gpu_counters", None), \
                contextlib.redirect_stdout(text):
            guards = [devrun.GpuGuard() for _ in range(3)]
            for g in guards:
                g.precheck(wait=5)
                g.start(1234)
                r = g.finish()
                self.assertIsNone(r["clean"])
                self.assertIn("not checked", devrun.gpu_summary(r))
        self.assertEqual(text.getvalue().count("not available"), 1)


class Bench(unittest.TestCase):
    """bench() with run() and the reports faked: which runs it does, in what order."""

    def bench(self, dirty, runs=2, warmup=1, retries=3):
        """dirty: tags whose run is contaminated."""
        order = []
        reports = {}

        def fake_run(a, tag):
            order.append(tag)
            other = 30.0 if tag in dirty else 0.5
            reports[tag] = assess(samples(10, game=90.0, other=other))
            return Path(tag)

        a = argparse.Namespace(configs="d3d12:1,vulkan:1", runs=runs, warmup=warmup,
                               name="b", gpu_retries=retries, gpu_guard=True, fixed_dt=0)
        stats = {"median_ms": 5.0, "low1_fps": 100.0, "mean_fps": 200.0, "seconds": 10.0}
        text = io.StringIO()
        with mock.patch.object(devrun, "run", fake_run), \
                mock.patch.object(devrun, "gpu_report", lambda out: reports[str(out)]), \
                mock.patch.object(devrun, "analyze", lambda out, quiet=False: {"drive": stats}), \
                mock.patch.object(devrun, "track", lambda out: {}), \
                mock.patch.object(devrun, "watch", lambda out: {}), \
                contextlib.redirect_stdout(text):
            devrun.bench(a)
        return order, text.getvalue()

    def test_clean(self):
        order, text = self.bench(set())
        self.assertEqual(order, ["b-d3d12-x1-0", "b-vulkan-x1-0", "b-d3d12-x1-1",
                                 "b-vulkan-x1-1", "b-d3d12-x1-2", "b-vulkan-x1-2"])
        self.assertIn("every run counted was clean", text)

    def test_redo_keeps_alternation(self):
        order, text = self.bench({"b-vulkan-x1-1", "b-vulkan-x1-1-redo1"})
        self.assertEqual(order, ["b-d3d12-x1-0", "b-vulkan-x1-0", "b-d3d12-x1-1",
                                 "b-vulkan-x1-1", "b-vulkan-x1-1-redo1", "b-vulkan-x1-1-redo2",
                                 "b-d3d12-x1-2", "b-vulkan-x1-2"])
        self.assertIn("2 run(s) redone", text)
        self.assertNotIn("NOT RELIABLE", text)
        self.assertEqual(text.count("runs 5.00, 5.00;"), 2)  # both sides: two clean runs

    def test_warmup_not_redone(self):
        order, text = self.bench({"b-d3d12-x1-0"})
        self.assertEqual(len(order), 6)
        self.assertIn("every run counted was clean", text)

    def test_cap(self):
        dirty = {f"b-d3d12-x1-{i}{s}" for i in (1, 2) for s in ("", "-redo1", "-redo2")}
        order, text = self.bench(dirty, retries=2)
        self.assertEqual(order, ["b-d3d12-x1-0", "b-vulkan-x1-0", "b-d3d12-x1-1",
                                 "b-d3d12-x1-1-redo1", "b-d3d12-x1-1-redo2", "b-vulkan-x1-1",
                                 "b-d3d12-x1-2", "b-vulkan-x1-2"])
        self.assertIn("NOT RELIABLE", text)
        self.assertIn("d3d12 x1: no clean runs", text)
        self.assertIn("forza.exe", text)


class GameArgs(unittest.TestCase):
    """game_args(): every option once, the last value winning (the SDK ignores the whole command
    line when one is repeated)."""

    def test_repeated_option_last_wins(self):
        a = argparse.Namespace(cache_root=None, backend="vulkan", res="1", fps="0", vsync="false",
                               fullscreen=True, intro="normal", script="", keep_open=False,
                               pipe="", fixed_dt=0, record=None, track=False,
                               extra=["--hdr=off", "--vsync=true", "--hdr=on"])
        with contextlib.redirect_stdout(io.StringIO()):
            args = devrun.game_args(a, Path("out"))
        keys = [x.split("=", 1)[0] for x in args]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertIn("--hdr=on", args)
        self.assertNotIn("--hdr=off", args)
        self.assertIn("--vsync=true", args)
        self.assertNotIn("--vsync=false", args)


if __name__ == "__main__":
    unittest.main()

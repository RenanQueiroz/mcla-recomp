"""Fits frame time = fixed + per_draw * draws over the 20-45 s part of each
REX_PERF_LOG_CSV run (uncapped), so runs whose title-screen views differ in
load can still be compared. Also prints the median frame time and draws."""
import csv, statistics, sys

def fit(xs, ys):
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return my - b * mx, b

print(f"{'run':<16}{'frames':>7}{'med ms':>8}{'med draws':>10}{'fixed ms':>9}{'us/draw':>9}{'ms @5000 draws':>16}")
for path in sys.argv[1:]:
    t, xs, ys = 0.0, [], []
    for r in csv.DictReader(open(path)):
        f = int(r["frame_time_us"]); t += f / 1e6
        d = int(r["draw_calls"])
        if 20 <= t < 45 and d > 500 and f < 100_000:
            xs.append(d); ys.append(f / 1000)
    a, b = fit(xs, ys)
    name = path.replace("\\", "/").rsplit("/", 1)[-1].removesuffix(".csv")
    print(f"{name:<16}{len(xs):>7}{statistics.median(ys):>8.2f}{statistics.median(xs):>10.0f}{a:>9.2f}{b * 1000:>9.3f}{a + b * 5000:>16.2f}")

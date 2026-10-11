"""Looks for clicks in scream_dump segments: sample-to-sample jumps far above the local level.

    click_scan.py capture-*.raw

For each segment: where the sound starts (first non-zero frame), and every jump that is both
> 2% of full scale and > 30x the median jump of the surrounding 20 ms. A click at a segment start
is reported with its distance from the start of the segment.
"""
import re
import statistics
import sys

for path in sys.argv[1:]:
    m = re.search(r"-(\d+)-(S16_LE|S24_3LE|S24_LE|S32_LE)\.raw$", path)
    if not m:
        continue
    rate, fmt = int(m.group(1)), m.group(2)
    bps = {"S16_LE": 2, "S24_3LE": 3, "S24_LE": 4, "S32_LE": 4}[fmt]
    full = float(1 << (8 * bps - 1))
    data = open(path, "rb").read()
    n = len(data) // (bps * 2)
    left = [int.from_bytes(data[(2 * i) * bps:(2 * i + 1) * bps], "little", signed=True) for i in range(n)]
    right = [int.from_bytes(data[(2 * i + 1) * bps:(2 * i + 2) * bps], "little", signed=True) for i in range(n)]
    first = next((i for i in range(n) if left[i] or right[i]), None)
    print(f"{path.split('/')[-1]}: {n / rate:.2f} s; first sound at {('%.1f ms' % (first * 1000 / rate)) if first is not None else 'never'}")
    win = max(1, rate // 50)
    for ch, x in (("L", left), ("R", right)):
        d = [abs(x[i] - x[i - 1]) for i in range(1, n)]
        hits = 0
        for i in range(1, len(d)):
            if d[i] < 0.02 * full:
                continue
            lo, hi = max(0, i - win), min(len(d), i + win)
            med = statistics.median(d[lo:hi]) or 1
            if d[i] > 30 * med:
                hits += 1
                if hits <= 5:
                    print(f"  {ch} jump {d[i] / full * 100:.1f}% FS at {i / rate * 1000:.1f} ms (local median {med / full * 100:.3f}%)")
        if hits > 5:
            print(f"  {ch}: {hits} jumps in all")

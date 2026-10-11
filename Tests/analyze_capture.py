"""Quick look at scream_dump segment files: does each one look like music at its stated format?

    analyze_capture.py capture-001-96000-S24_3LE.raw [...]

Per segment: duration, peak and RMS level (dBFS), how often the low byte is zero (a 16-bit source
sent as 24-bit has it always zero) and "roughness" = mean |x[n] - x[n-1]| / mean |x|: music is
smooth (well under 0.5); white noise is ~1.1-1.4; samples read at the wrong width or offset look
like noise.
"""
import math
import re
import sys

for path in sys.argv[1:]:
    m = re.search(r"-(\d+)-(S16_LE|S24_3LE|S24_LE|S32_LE)\.raw$", path)
    if not m:
        print(f"{path}: name not from scream_dump")
        continue
    rate, fmt = int(m.group(1)), m.group(2)
    bps = {"S16_LE": 2, "S24_3LE": 3, "S24_LE": 4, "S32_LE": 4}[fmt]
    data = open(path, "rb").read()
    n = len(data) // bps
    step = max(1, n // 400000)          # sample at most ~400k values
    vals, lowzero = [], 0
    for i in range(0, n, step):
        b = data[i * bps:(i + 1) * bps]
        v = int.from_bytes(b, "little", signed=True)
        vals.append(v)
        if bps >= 3 and b[0] == 0:
            lowzero += 1
    full = float(1 << (8 * bps - 1))
    peak = max((abs(v) for v in vals), default=0)
    rms = math.sqrt(sum(v * v for v in vals) / max(1, len(vals)))
    # roughness on consecutive samples of one channel (left = even samples)
    left = [int.from_bytes(data[i * bps:(i + 1) * bps], "little", signed=True) for i in range(0, min(n, 200000), 2)]
    meanabs = sum(abs(v) for v in left) / max(1, len(left))
    rough = (sum(abs(left[i] - left[i - 1]) for i in range(1, len(left))) / max(1, len(left) - 1)) / meanabs if meanabs else 0
    db = lambda x: 20 * math.log10(x / full) if x > 0 else -999
    secs = n / 2 / rate
    print(f"{path.split('/')[-1]}: {secs:.2f} s, peak {db(peak):.1f} dBFS, rms {db(rms):.1f} dBFS, "
          f"low byte zero {100 * lowzero / max(1, len(vals)):.0f}%, roughness {rough:.2f}")

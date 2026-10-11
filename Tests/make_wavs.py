"""Writes stereo test WAVs (full-scale white noise: every bit matters) for the end-to-end test.

    make_wavs.py outdir
"""
import os
import random
import struct
import sys
import wave

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/screammusic-wavs"
os.makedirs(out, exist_ok=True)
random.seed(7)
for rate, bits in [(44100, 16), (48000, 24), (96000, 24), (192000, 24), (88200, 16), (44100, 24)]:
    frames = rate * 3
    bps = bits // 8
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    data = bytearray()
    for _ in range(frames * 2):
        data += struct.pack("<i", random.randint(lo, hi))[:bps]
    path = f"{out}/noise-{rate}-{bits}.wav"
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(bps)
        w.setframerate(rate)
        w.writeframes(bytes(data))
    print(path)

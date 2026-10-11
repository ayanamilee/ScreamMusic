"""Checks a scream_dump segment against the WAV that was played.

    compare.py file.wav segment.raw wire_bits

The segment must contain the WAV's samples, converted to the wire depth (wider = shifted up,
as the driver does), contiguously and unchanged; anything before them and after them must be
digital silence (start-up buffers, payload padding).
"""
import sys
import wave

wav, raw, wire_bits = sys.argv[1], sys.argv[2], int(sys.argv[3])
with wave.open(wav, "rb") as w:
    src_bits = w.getsampwidth() * 8
    src = w.readframes(w.getnframes())
got = open(raw, "rb").read()

sb, wb = src_bits // 8, wire_bits // 8
if src_bits == wire_bits:
    want = src
else:
    shift = wire_bits - src_bits
    out = bytearray()
    for i in range(0, len(src), sb):
        v = int.from_bytes(src[i:i + sb], "little", signed=True) << shift
        out += v.to_bytes(wb, "little", signed=True)
    want = bytes(out)

probe = want[:4096]
start = got.find(probe)
if start < 0:
    print(f"FAIL {raw}: the file's first samples are not in the capture ({len(got)} bytes)")
    sys.exit(1)
if any(got[:start]):
    print(f"FAIL {raw}: non-silent bytes before the audio (offset {start})")
    sys.exit(1)
seg = got[start:start + len(want)]
if seg != want:
    bad = next((i for i in range(min(len(seg), len(want))) if seg[i] != want[i]), min(len(seg), len(want)))
    print(f"FAIL {raw}: differs at byte {bad} of {len(want)} (captured {len(seg)})")
    sys.exit(1)
if any(got[start + len(want):]):
    print(f"FAIL {raw}: non-silent bytes after the audio")
    sys.exit(1)
print(f"ok   {wav.split('/')[-1]} -> {wire_bits}-bit: {len(want)} bytes bit-exact (lead-in {start} bytes of silence)")

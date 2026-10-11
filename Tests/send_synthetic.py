"""Sends a fixed synthetic Scream stream to 127.0.0.1:<port> and writes the expected payloads.

50 packets 44.1 kHz S16_LE, an end-of-track marker, then 20 packets 96 kHz S24_3LE
(extended header). Used by test_dump.sh to check scream_dump end to end.
"""
import random
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 4011
out = sys.argv[2] if len(sys.argv) > 2 else "/tmp"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
random.seed(1)
data = bytes(random.getrandbits(8) for _ in range(1152 * 50))


def hdr(b0, b1, b4, b5):
    return bytes([b0, b1, 2, 3, b4, b5])


for i in range(50):
    s.sendto(hdr(1, 16, 0x10, 0) + data[i * 1152:(i + 1) * 1152], ("127.0.0.1", port))
    time.sleep(0.002)
s.sendto(hdr(1, 16, 0x90, 0), ("127.0.0.1", port))
for i in range(20):
    s.sendto(hdr(2, 24, 0, 0) + data[i * 1152:(i + 1) * 1152], ("127.0.0.1", port))
    time.sleep(0.002)
open(f"{out}/expect-001.raw", "wb").write(data)
open(f"{out}/expect-002.raw", "wb").write(data[:1152 * 20])

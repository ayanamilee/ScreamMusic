#!/bin/sh
# End-to-end check of scream_dump: synthetic stream in, segment files out, byte compare.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
build="$here/../build"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
port=${PORT:-14011}

"$build/scream_dump" -p "$port" -o "$tmp/sd" -q > "$tmp/log" &
pid=$!
sleep 1
python3 "$here/send_synthetic.py" "$port" "$tmp"
sleep 0.5
kill -INT $pid; wait $pid || true

cmp "$tmp/sd-001-44100-S16_LE.raw" "$tmp/expect-001.raw"
cmp "$tmp/sd-002-96000-S24_3LE.raw" "$tmp/expect-002.raw"
grep -q "end-of-track markers: 1" "$tmp/log"
echo "scream_dump end-to-end test passed"

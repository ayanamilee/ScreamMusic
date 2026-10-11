#!/bin/sh
# End-to-end through the real HAL (needs the installed driver): hogplay plays test WAVs into
# the ScreamMusic device, scream_dump receives on 127.0.0.1, compare.py checks every byte.
# Points the driver at 127.0.0.1:$PORT for the run and restores the destination afterwards.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
build="$here/../build"
port=${PORT:-14013}
wavs=$(mktemp -d)
cap=$(mktemp -d)
dump=
saved=$("$build/smctl" dest | plutil -convert json -o - -- - 2>/dev/null || true)
restore() {
    [ -n "$dump" ] && kill $dump 2>/dev/null || true
    if [ -n "$saved" ]; then
        ip=$(printf '%s' "$saved" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["ip"], d["port"], d["header"])')
        "$build/smctl" dest $ip >/dev/null && echo "destination restored: $ip"
    fi
    rm -rf "$wavs" "$cap"
}
trap restore EXIT

python3 "$here/make_wavs.py" "$wavs" >/dev/null
"$build/smctl" dest 127.0.0.1 "$port" extended
"$build/smctl" bits 0

"$build/scream_dump" -p "$port" -o "$cap/c" -q > "$cap/log" &
dump=$!
sleep 1

fail=0
n=0
run() {  # mode wav wire_bits
    n=$((n + 1))
    "$build/hogplay" -m "$1" "$2" > "$cap/play-$n.log" || { echo "FAIL hogplay $1 $2"; cat "$cap/play-$n.log"; fail=1; return; }
    sleep 1
    seg=$(ls "$cap"/c-*.raw | sort | tail -1)
    python3 "$here/compare.py" "$2" "$seg" "$3" || fail=1
}

for f in "$wavs"/noise-*.wav; do
    bits=$(basename "$f" .wav | cut -d- -f3)
    run int "$f" "$bits"          # hog + integer: passthrough at the file's depth
    run float "$f" 24             # Float32 (mixable): 24-bit on the wire (SMbd = auto, P1)
done
kill -INT $dump; wait $dump || true
grep -E "segment|end-of-track markers" "$cap/log" | tail -3
[ $fail = 0 ] && echo "HAL end-to-end: all cases bit-exact" || { echo "HAL end-to-end: FAILURES"; exit 1; }

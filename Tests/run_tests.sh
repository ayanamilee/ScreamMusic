#!/bin/sh
# Builds and runs the protocol tests. The differential test compiles scream_fill_header()
# straight out of ../../snd-screamalsa.c, so it fails if the kernel driver's header changes.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
kernel="$root/../../snd-screamalsa.c"
build="$root/build"
mkdir -p "$build"

for f in scream_pcm_wire_bits scream_pcm_wire_layout scream_header_original scream_fill_header; do
    awk -v name="$f" '$0 ~ "^static .*"name"\\(" {p=1} p {print} p && /^}/ {p=0; print ""}' "$kernel"
done > "$build/kernel_header_snippet.c"
grep -q "scream_fill_header" "$build/kernel_header_snippet.c" || { echo "could not extract scream_fill_header from $kernel"; exit 1; }

CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror"
clang $CFLAGS -o "$build/test_protocol" "$here/test_protocol.c"
clang $CFLAGS -Wno-unused-function -I"$build" -o "$build/test_vs_kernel" "$here/test_vs_kernel.c"
"$build/test_protocol"
"$build/test_vs_kernel"
clang $CFLAGS -o "$build/scream_dump" "$root/Tools/scream_dump/scream_dump.c"
"$here/test_dump.sh"
"$root/Driver/build.sh" "$build" >/dev/null 2>&1
clang $CFLAGS -framework CoreAudio -framework CoreFoundation -o "$build/driver_harness" "$here/driver_harness.c"
"$build/driver_harness" "$build/ScreamMusic.driver"
clang $CFLAGS -framework CoreAudio -framework CoreFoundation -o "$build/hogplay" "$root/Tools/hogplay/hogplay.c"
clang $CFLAGS -Wno-four-char-constants -framework CoreAudio -framework CoreFoundation -o "$build/smctl" "$root/Tools/smctl/smctl.c"
echo "(HAL end-to-end test needs the installed driver: Tests/e2e_hal.sh)"

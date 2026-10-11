#!/bin/sh
# Builds ScreamMusic.driver (universal, ad-hoc signed) into the directory given as $1
# (default: ../build). Install by hand (admin):
#   sudo ditto ScreamMusic.driver /Library/Audio/Plug-Ins/HAL/ScreamMusic.driver && sudo killall coreaudiod
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-"$here/../build"}
mkdir -p "$out"
drv="$out/ScreamMusic.driver"
rm -rf "$drv"
mkdir -p "$drv/Contents/MacOS"
clang -bundle -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
    -mmacosx-version-min=26.0 -arch arm64 -arch x86_64 \
    -framework CoreAudio -framework CoreFoundation \
    -o "$drv/Contents/MacOS/ScreamMusic" "$here/ScreamMusicDriver.c"
cp "$here/Info.plist" "$drv/Contents/Info.plist"
plutil -lint "$drv/Contents/Info.plist" >/dev/null
codesign --force --sign - --timestamp=none "$drv"
chmod -R a+rX "$drv"   # coreaudiod (_coreaudiod) must be able to read it

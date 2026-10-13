#!/bin/sh
# Builds build/ScreamMusic.app: the SwiftPM menu-bar app with ScreamMusic.driver
# (Driver/, from P1 on) embedded in Contents/Resources. Ad-hoc signed.
#   ./build.sh          release build
#   DEBUG=1 ./build.sh  debug build
set -eu
root=$(cd "$(dirname "$0")" && pwd)
cd "$root"
config=release
[ "${DEBUG:-0}" = 1 ] && config=debug

swift build -c "$config"
bin=$(swift build -c "$config" --show-bin-path)

app="$root/build/ScreamMusic.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin/ScreamMusic" "$app/Contents/MacOS/ScreamMusic"
cp "$root/App/Resources/Info.plist" "$app/Contents/Info.plist"
[ -f "$root/App/Resources/AppIcon.icns" ] && \
    cp "$root/App/Resources/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"
for f in "$root/App/Resources"/MenuBarIcon*.png; do
    [ -f "$f" ] && cp "$f" "$app/Contents/Resources/"
done

if [ -x "$root/Driver/build.sh" ]; then
    "$root/Driver/build.sh" "$root/build"
    ditto "$root/build/ScreamMusic.driver" "$app/Contents/Resources/ScreamMusic.driver"
fi

codesign --force --sign - --timestamp=none "$app"
echo "built $app"

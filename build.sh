#!/bin/sh
# Builds everything on the host: the emulators (toolchain/setup.sh, skips
# what is there) and RetroDeck itself (build/bin/retrodeck, retrodeck-amiga).
# Works from VS Code's Flatpak terminal too.

set -e
HERE=$(cd "$(dirname "$0")" && pwd)

if [ -f /.flatpak-info ]; then
    exec flatpak-spawn --host --directory="$HERE" "$HERE/build.sh" "$@"
fi

"$HERE/toolchain/setup.sh"

GENERATOR=
command -v ninja >/dev/null && GENERATOR="-G Ninja"
[ -f "$HERE/build/CMakeCache.txt" ] && GENERATOR=
cmake -S "$HERE" -B "$HERE/build" $GENERATOR -DCMAKE_BUILD_TYPE=Release
cmake --build "$HERE/build" -j"$(nproc)"
echo "RetroDeck: $HERE/retrodeck.sh"

#!/bin/sh
# Starts RetroDeck. This is the file to add to Steam as a non-Steam game.
# Arguments are passed on (e.g. --window 1920x1080).

HERE=$(cd "$(dirname "$0")" && pwd)

# From VS Code's Flatpak terminal: start it on the host, like Steam does
if [ -f /.flatpak-info ]; then
    exec flatpak-spawn --host "$HERE/retrodeck.sh" "$@"
fi

if [ ! -x "$HERE/build/bin/retrodeck" ]; then
    echo "RetroDeck is not built yet: $HERE/build.sh" >&2
    exit 1
fi
exec "$HERE/build/bin/retrodeck" "$@"

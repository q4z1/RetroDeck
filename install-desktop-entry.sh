#!/bin/sh
# Puts RetroDeck into the desktop's application menu
# (~/.local/share/applications/retrodeck.desktop). Steam's "Add a Non-Steam
# Game" lists it from there, so it can be added to Steam (and Game Mode)
# with one tick. --remove takes the entry away again.

HERE=$(cd "$(dirname "$0")" && pwd)
ENTRY="$HOME/.local/share/applications/retrodeck.desktop"

if [ "$1" = "--remove" ]; then
    rm -f "$ENTRY"
    exit 0
fi

mkdir -p "$(dirname "$ENTRY")"
cat >"$ENTRY" <<EOF
[Desktop Entry]
Type=Application
Name=RetroDeck
Comment=Plus/4, C64, Amiga and MEGA65 games with a controller
Exec="$HERE/retrodeck.sh"
Path=$HERE
Terminal=false
Categories=Game;Emulator;
EOF
echo "$ENTRY"

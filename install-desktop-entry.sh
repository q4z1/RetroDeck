#!/bin/sh
# Puts RetroDeck into the desktop's application menu, under Games, with its
# icon (~/.local/share/applications/retrodeck.desktop). Steam's "Add a Non-Steam
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
GenericName=Game Launcher
GenericName[de]=Spiele-Launcher
Comment=Plus/4, C64, Amiga and MEGA65 games with a controller
Comment[de]=Plus/4-, C64-, Amiga- und MEGA65-Spiele mit dem Controller
Exec="$HERE/retrodeck.sh"
Path=$HERE
Icon=$HERE/assets/icon/retrodeck.png
StartupWMClass=retrodeck
Terminal=false
Categories=Game;Emulator;
EOF
echo "$ENTRY"

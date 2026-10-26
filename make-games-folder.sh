#!/bin/sh
# Lays out a folder for RetroDeck's games, a folder per system:
#
#   <target>/Plus4    PRG D64 D81 T64 TAP           (also in ZIPs)
#   <target>/C64      PRG D64 D71 D81 G64 T64 TAP CRT
#   <target>/Amiga    ADF ADZ DMS, and your Kickstart ROM (*kick*.rom)
#   <target>/MEGA65   PRG D81, with the files a game loads from the SD card
#
#   ./make-games-folder.sh ~/Games/Retro          only the folders
#   ./make-games-folder.sh --use /mnt/sd/Retro    and library.conf points there
#
# --use replaces the folder of library.conf's lines for the four systems
# that end in /Plus4, /C64, /Amiga or /MEGA65 (options such as exclude= stay),
# and adds a line for a system that has none. Other lines are left alone.
# Then: Menu -> "Bibliothek neu einlesen" (or start RetroDeck again).

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
USE=
if [ "$1" = "--use" ]; then
    USE=1
    shift
fi
if [ $# -ne 1 ]; then
    echo "usage: $0 [--use] <target folder>" >&2
    exit 1
fi

mkdir -p "$1"
TARGET=$(cd "$1" && pwd)
for folder in Plus4 C64 Amiga MEGA65; do
    mkdir -p "$TARGET/$folder"
    echo "$TARGET/$folder"
done

[ -n "$USE" ] || exit 0

# library.conf names folders under the home folder with ~
case "$TARGET" in
    "$HOME"/*) SHOWN="~${TARGET#"$HOME"}" ;;
    *) SHOWN=$TARGET ;;
esac
CONF="$HERE/library.conf"
awk -v target="$SHOWN" '
    BEGIN { folder["plus4"] = "Plus4"; folder["c64"] = "C64"; folder["amiga"] = "Amiga"; folder["mega65"] = "MEGA65" }
    # A system line whose folder ends in the standard name gets the new place
    $1 in folder && $2 ~ ("/" folder[$1] "/?$") && !($1 in done) {
        rest = $0
        sub(/^[ \t]*[^ \t]+[ \t]+[^ \t]+/, "", rest)
        printf "%-7s %s%s\n", $1, target "/" folder[$1], rest
        done[$1] = 1
        next
    }
    { print }
    END {
        for (id in folder) if (!(id in done)) printf "%-7s %s\n", id, target "/" folder[id]
    }
' "$CONF" >"$CONF.new"
mv "$CONF.new" "$CONF"
echo "library.conf now uses $SHOWN"

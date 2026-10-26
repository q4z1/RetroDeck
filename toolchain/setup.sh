#!/bin/sh
# Builds RetroDeck's emulators into this folder, on the host (they run from
# Steam's Game Mode, outside any Flatpak), from sources on the net at pinned
# versions. What is there and up to date is skipped; a component is built
# anew when its pinned commit or one of its patches changes.
#
#   vice/        VICE 3.10 (x64sc, xplus4), SDL2 user interface   SourceForge
#   bin/yapesdl  YapeSDL (Plus/4)                                github.com/q4z1/yapesdl
#   bin/xmega65  Xemu (MEGA65) + patches/xemu-*.patch            github.com/q4z1/xemu
#   src/vAmiga   the vAmiga core for the Amiga front end         github.com/dirkwhoffmann/vAmiga
#   rom/         MEGA65.ROM, built from the C65 ROM in Cloanto's C64 Forever
#                and the MEGA65 patch 920413 (files.mega65.org)
#
# Not from the net: the Amiga's Kickstart ROM, which may not be passed on.
# Your own copy goes into the Amiga folder of the library (library.conf,
# ~/Games/Retro/Amiga), where the launcher looks for it.
#
# Run from anywhere, also from VS Code's Flatpak terminal:
#
#   toolchain/setup.sh            all components
#   toolchain/setup.sh vice rom   only these (vice yapesdl xemu vamiga rom)

set -e

HERE=$(cd "$(dirname "$0")" && pwd)

# Inside a Flatpak sandbox, run on the host instead: the binaries have to
# link against the host's libraries
if [ -f /.flatpak-info ]; then
    exec flatpak-spawn --host --directory="$HERE" "$HERE/setup.sh" "$@"
fi

DL="$HERE/downloads"
SRC="$HERE/src"
JOBS=$(nproc)

VICE_VERSION=3.10
XA_VERSION=2.4.1
YAPE_REPO=https://github.com/q4z1/yapesdl.git
YAPE_COMMIT=7153de9e001d6ba57fd9d0d2b0d43b92773b53ca   # feature/start-fullscreen
XEMU_REPO=https://github.com/q4z1/xemu.git
XEMU_COMMIT=658b7f43fa78f44d0947f2731bd968cf270b51f6   # audio-sync
VAMIGA_REPO=https://github.com/dirkwhoffmann/vAmiga.git
VAMIGA_COMMIT=c38fa527ea97b9b0036c0e58526cf2714fd97213
CABEXTRACT_VERSION=1.11
OLEFILE_TAG=v0.47
MSI_URL=https://cdn.cloanto.com/pub/c64forever/c64-forever-11-setup.msi
ROM_PATCH_URL=https://files.mega65.org/files/other/920413_Sn7YEw.zip
C65_SHA1=b63d970727a2b8da72a0a8e234f3c30a20cbcb26
MEGA65_SHA1=4214ba4615a6ad852ddfc48b51a1d0ceca34a622

COMPONENTS="$*"
want() {
    [ -z "$COMPONENTS" ] && return 0
    case " $COMPONENTS " in *" $1 "*) return 0 ;; esac
    return 1
}
step() { printf '\n== %s\n' "$*"; }
fail() { echo "Error: $*" >&2; exit 1; }

# What the build needs on the system; nothing is installed here, only named
missing=
need() { command -v "$1" >/dev/null 2>&1 || missing="$missing $2"; }
need gcc gcc
need g++ gcc
need make make
need cmake cmake
need pkg-config pkgconf
need git git
need curl curl
need flex flex
need bison bison
need python3 python
need bsdtar libarchive
need sdl2-config sdl2-compat
pkg-config --exists sdl2 2>/dev/null || missing="$missing sdl2-compat"
pkg-config --exists zlib 2>/dev/null || missing="$missing zlib"
if [ -n "$missing" ]; then
    packages=$(printf '%s\n' $missing | sort -u | tr '\n' ' ')
    echo "Missing on this system: $packages" >&2
    echo "On Arch/CachyOS:  sudo pacman -S --needed $packages" >&2
    exit 1
fi

mkdir -p "$DL" "$SRC" "$HERE/bin" "$HERE/rom"

download() { # URL FILE
    [ -f "$2" ] && return 0
    curl -L --fail --progress-bar -o "$2.part" "$1" && mv "$2.part" "$2"
}

# A git checkout of REPO at COMMIT in DIR; false if the commit cannot be had
checkout() { # REPO COMMIT DIR
    if [ ! -d "$3/.git" ]; then
        rm -rf "$3"
        git clone -q "$1" "$3" || return 1
    fi
    git -C "$3" remote set-url origin "$1"
    git -C "$3" cat-file -e "$2^{commit}" 2>/dev/null ||
        git -C "$3" fetch -q origin "$2" 2>/dev/null ||
        git -C "$3" fetch -q origin 2>/dev/null || true
    git -C "$3" cat-file -e "$2^{commit}" 2>/dev/null || return 1
    git -C "$3" checkout -q -f "$2"
}

# What a component was built from (commit and patches), kept beside it, so
# that a new pin or patch builds it again
stamp() { # COMMIT PATCH...
    { echo "$1"; shift; for f; do [ -f "$f" ] && cat "$f"; done; } | sha1sum | cut -c1-40
}
up_to_date() { # BINARY STAMP
    [ -x "$1" ] && [ "$(cat "$1.stamp" 2>/dev/null)" = "$2" ]
}
# The commit is not to be had (not pushed yet, or no network): keep a
# build that is there, stop without one
unavailable() { # NAME COMMIT REPO BINARY
    if [ -x "$4" ]; then
        echo "Warning: $1 commit $2 is not on $3 (not pushed yet, or offline);" >&2
        echo "         keeping the $1 that is built." >&2
        return 0
    fi
    fail "$1 commit $2 is not on $3 (not pushed yet, or offline)"
}

if want vice && [ ! -x "$HERE/vice/bin/x64sc" ]; then
    step "VICE $VICE_VERSION"
    tarball="$DL/vice-$VICE_VERSION.tar.gz"
    download "https://downloads.sourceforge.net/project/vice-emu/releases/vice-$VICE_VERSION.tar.gz" "$tarball"
    # VICE's configure insists on dos2unix (it only converts text files); a
    # stand-in, so nothing has to be installed on the system
    mkdir -p "$SRC/shims"
    cat >"$SRC/shims/dos2unix" <<'EOS'
#!/bin/sh
[ $# -eq 0 ] && exec sed 's/\r$//'
for f; do case "$f" in -*) ;; *) sed -i 's/\r$//' "$f" ;; esac; done
EOS
    chmod +x "$SRC/shims/dos2unix"
    # ... and on the xa 6502 assembler, for the drive and cartridge code it
    # builds; built here as well, only for this build
    if [ ! -x "$SRC/shims/xa" ]; then
        xatar="$DL/xa-$XA_VERSION.tar.gz"
        download "https://www.floodgap.com/retrotech/xa/dists/xa-$XA_VERSION.tar.gz" "$xatar"
        rm -rf "$SRC/xa-$XA_VERSION"
        tar -xzf "$xatar" -C "$SRC"
        make -C "$SRC/xa-$XA_VERSION" xa >"$SRC/xa-build.log" 2>&1 ||
            { tail -20 "$SRC/xa-build.log"; exit 1; }
        cp "$SRC/xa-$XA_VERSION/xa" "$SRC/shims/xa"
    fi
    PATH="$SRC/shims:$PATH"
    rm -rf "$SRC/vice-$VICE_VERSION"
    tar -xzf "$tarball" -C "$SRC"
    cd "$SRC/vice-$VICE_VERSION"
    ./configure --prefix="$HERE/vice" --enable-sdl2ui --without-oss \
        --disable-html-docs --disable-pdf-docs >"$SRC/vice-configure.log" 2>&1 ||
        { tail -30 "$SRC/vice-configure.log"; exit 1; }
    make -j"$JOBS" >"$SRC/vice-build.log" 2>&1 || { tail -40 "$SRC/vice-build.log"; exit 1; }
    make install >"$SRC/vice-install.log" 2>&1 || { tail -20 "$SRC/vice-install.log"; exit 1; }
    cd "$HERE"
fi

if want yapesdl; then
    s=$(stamp "$YAPE_COMMIT")
    if ! up_to_date "$HERE/bin/yapesdl" "$s"; then
        step "YapeSDL"
        if checkout "$YAPE_REPO" "$YAPE_COMMIT" "$SRC/yapesdl"; then
            make -C "$SRC/yapesdl" -j"$JOBS" yape >"$SRC/yapesdl-build.log" 2>&1 ||
                { tail -30 "$SRC/yapesdl-build.log"; exit 1; }
            cp "$SRC/yapesdl/yapesdl" "$HERE/bin/yapesdl"
            echo "$s" >"$HERE/bin/yapesdl.stamp"
        else
            unavailable YapeSDL "$YAPE_COMMIT" "$YAPE_REPO" "$HERE/bin/yapesdl"
        fi
    fi
fi

if want xemu; then
    # Changes for RetroDeck, in this copy only (patches/):
    #   xemu-centered-window       the window opens centered, so that the
    #                              launcher's screen (SDL_VIDEO_DISPLAY_PRIORITY)
    #                              decides where, not the window manager
    #   xemu-hdos-readfile-buffer  -hdosvirt's readfile maps the SD-card
    #                              buffer at $DE00 as Hyppo does (Space Commander
    #                              read the D81's buffer instead)
    s=$(stamp "$XEMU_COMMIT" "$HERE"/patches/xemu-*.patch)
    if ! up_to_date "$HERE/bin/xmega65" "$s"; then
        step "Xemu (MEGA65)"
        if checkout "$XEMU_REPO" "$XEMU_COMMIT" "$SRC/xemu"; then
            for patch in "$HERE"/patches/xemu-*.patch; do
                git -C "$SRC/xemu" apply "$patch"
            done
            make -C "$SRC/xemu/targets/mega65" -j"$JOBS" >"$SRC/xemu-build.log" 2>&1 ||
                { tail -30 "$SRC/xemu-build.log"; exit 1; }
            cp "$SRC/xemu/build/bin/xmega65.native" "$HERE/bin/xmega65"
            echo "$s" >"$HERE/bin/xmega65.stamp"
        else
            unavailable Xemu "$XEMU_COMMIT" "$XEMU_REPO" "$HERE/bin/xmega65"
        fi
    fi
fi

if want vamiga; then
    if [ "$(git -C "$SRC/vAmiga" rev-parse HEAD 2>/dev/null)" != "$VAMIGA_COMMIT" ]; then
        step "vAmiga core"
        checkout "$VAMIGA_REPO" "$VAMIGA_COMMIT" "$SRC/vAmiga" ||
            fail "vAmiga commit $VAMIGA_COMMIT could not be fetched from $VAMIGA_REPO"
    fi
fi

if want rom && [ ! -f "$HERE/rom/MEGA65.ROM" ]; then
    # What mega65go's SETUP.cmd does on Windows: the C65 ROM 910828 out of
    # Cloanto's C64 Forever (free for personal use; it may not be passed on,
    # only the patch is published), patched with 920413.bdf
    step "MEGA65.ROM"
    echo "Downloading C64 Forever (268 MB) and the ROM patch ..."
    download "$MSI_URL" "$DL/c64-forever-11-setup.msi"
    download "$ROM_PATCH_URL" "$DL/920413_Sn7YEw.zip"
    # Tools for it: cabextract (bsdtar fails on these CABs' LZX) and the
    # Python module olefile (an MSI is an OLE compound file)
    if [ ! -x "$SRC/shims/cabextract" ]; then
        download "https://www.cabextract.org.uk/cabextract-$CABEXTRACT_VERSION.tar.gz" "$DL/cabextract-$CABEXTRACT_VERSION.tar.gz"
        rm -rf "$SRC/cabextract-$CABEXTRACT_VERSION"
        tar -xzf "$DL/cabextract-$CABEXTRACT_VERSION.tar.gz" -C "$SRC"
        (cd "$SRC/cabextract-$CABEXTRACT_VERSION" && ./configure -q && make -s) >"$SRC/cabextract-build.log" 2>&1 ||
            { tail -20 "$SRC/cabextract-build.log"; exit 1; }
        mkdir -p "$SRC/shims"
        cp "$SRC/cabextract-$CABEXTRACT_VERSION/cabextract" "$SRC/shims/cabextract"
    fi
    if [ ! -f "$SRC/python/olefile/olefile.py" ]; then
        mkdir -p "$SRC/python/olefile"
        for f in __init__.py olefile.py; do
            download "https://raw.githubusercontent.com/decalage2/olefile/$OLEFILE_TAG/olefile/$f" "$SRC/python/olefile/$f"
        done
    fi
    work="$SRC/rom-work"
    rm -rf "$work"
    mkdir -p "$work"
    PYTHONPATH="$SRC/python" python3 "$HERE/tools/msi-extract-cabs.py" "$DL/c64-forever-11-setup.msi" "$work/cab" >/dev/null
    for cab in "$work/cab"/*.cab; do
        "$SRC/shims/cabextract" -q -d "$work/files" "$cab"
    done
    # In MSI 11 it is file_Shared_54; found by its checksum
    c65=
    for f in "$work/files"/*; do
        [ "$(stat -c %s "$f")" = 131072 ] || continue
        if [ "$(sha1sum "$f" | cut -d' ' -f1)" = "$C65_SHA1" ]; then
            c65=$f
            break
        fi
    done
    [ -n "$c65" ] || fail "the C65 ROM 910828 is not in the MSI"
    bsdtar -xf "$DL/920413_Sn7YEw.zip" -C "$work"
    python3 -I "$HERE/tools/bspatch.py" "$c65" "$work/MEGA65.ROM" "$work/920413/920413.bdf"
    [ "$(sha1sum "$work/MEGA65.ROM" | cut -d' ' -f1)" = "$MEGA65_SHA1" ] ||
        fail "MEGA65.ROM does not have the expected checksum"
    mv "$work/MEGA65.ROM" "$HERE/rom/MEGA65.ROM"
    rm -rf "$work"
fi

# The Kickstart: only looked for, in the Amiga folders of library.conf
if want rom; then
    found=
    for dir in $(awk '$1 == "amiga" { print $2 }' "$HERE/../library.conf" | sed "s|^~|$HOME|"); do
        ls "$dir"/*[Kk]ick* >/dev/null 2>&1 && found=$dir && break
    done
    [ -n "$found" ] ||
        echo "Note: no Kickstart ROM in the Amiga folder (~/Games/Retro/Amiga); put your own copy there for Amiga games." >&2
fi

step "done"

#!/bin/bash
set -e

# Build the Dreamcast target and boot it in an emulator — the Mac smoke
# test for the console port.
#
#   ./test-dreamcast.sh            build, then run the disc image
#   ./test-dreamcast.sh -n         skip the build (run the existing
#                                  build/dreamcast/ltop.cdi)
#   ./test-dreamcast.sh -e <emu>   force the emulator (redream|flycast)
#
# Emulator: whatever it finds first — Flycast, then redream (an app in
# /Applications is enough). Neither installed and brew present? It offers
#   brew install --cask flycast
# Otherwise grab a build from https://flycast.github.io (or redream.io).
#
# What you should see: the disc boots straight into the game window
# (896x448 canvas scaled by the emulator) — title screen, then a room.
# The romdisk carries assets/, so textures/music play from the disc
# image like the real hardware would read them.
#
# BIOS: redream and Flycast both ship an HLE BIOS that boots homebrew
# discs fine — no BIOS dump needed. If you drop real firmware in and
# things behave differently, that's the variable.

cd "$(dirname "$0")"

SKIP_BUILD=0
EMU=""
while [ $# -gt 0 ]; do
    case "$1" in
        -n|--no-build) SKIP_BUILD=1 ;;
        -e|--emulator) EMU="$2"; shift ;;
        *) echo "usage: $0 [-n|--no-build] [-e|--emulator redream|flycast]"; exit 1 ;;
    esac
    shift
done

CDI="build/dreamcast/ltop.cdi"

if [ "$SKIP_BUILD" -eq 0 ]; then
    ./build-dreamcast.sh
elif [ ! -f "$CDI" ]; then
    echo "error: $CDI not found — run without -n to build it first"
    exit 1
fi

# ---- find an emulator -------------------------------------------------------

find_flycast() {
    for candidate in \
        /Applications/Flycast.app/Contents/MacOS/flycast \
        /Applications/Flycast.app/Contents/MacOS/Flycast \
        "$HOME/Applications/Flycast.app/Contents/MacOS/flycast" \
        "$HOME/Applications/Flycast.app/Contents/MacOS/Flycast"
    do
        [ -x "$candidate" ] && { echo "$candidate"; return 0; }
    done
    command -v flycast 2>/dev/null && return 0
    return 1
}

find_redream() {
    for candidate in \
        /Applications/redream.app/Contents/MacOS/redream \
        "$HOME/Applications/redream.app/Contents/MacOS/redream"
    do
        [ -x "$candidate" ] && { echo "$candidate"; return 0; }
    done
    return 1
}

EMU_BIN=""
case "$EMU" in
    flycast) EMU_BIN="$(find_flycast || true)"
        [ -z "$EMU_BIN" ] && { echo "error: flycast not found"; exit 1; } ;;
    redream) EMU_BIN="$(find_redream || true)"
        [ -z "$EMU_BIN" ] && { echo "error: redream not found"; exit 1; } ;;
    "")  if EMU_BIN="$(find_flycast)"; then
             :
         elif EMU_BIN="$(find_redream)"; then
             :
         elif command -v brew >/dev/null 2>&1; then
             echo "no emulator found — install one? (brew install --cask flycast)"
             printf "install now? [y/N] "
             read -r answer
             if [ "$answer" = "y" ] || [ "$answer" = "Y" ]; then
                 brew install --cask flycast
                 EMU_BIN="$(find_flycast)"
             fi
         fi
         [ -z "$EMU_BIN" ] && { echo "error: no Dreamcast emulator installed"; exit 1; } ;;
    *) echo "error: unknown emulator '$EMU' (redream|flycast)"; exit 1 ;;
esac

# ---- boot the disc ----------------------------------------------------------

case "$EMU_BIN" in
    *.app/*) EMU_NAME="$(basename "${EMU_BIN%%/Contents/*}")"; EMU_NAME="${EMU_NAME%.app}" ;;
    *) EMU_NAME="$(basename "$EMU_BIN")" ;;
esac
echo "booting $CDI in ${EMU_NAME} (ctrl-C here stops the emulator)..."
exec "$EMU_BIN" "$CDI"

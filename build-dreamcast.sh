#!/bin/bash
set -e

# Build the game for the Sega Dreamcast (KallistiOS + raylib4Dreamcast).
# Everything cross-compile-related runs inside Docker (OrbStack works great);
# the first run builds the toolchain image (~2 GB download), later runs are
# cached and take about a minute.
#
# Output: build/dreamcast/ltop.elf  (dcload-able)
#         build/dreamcast/ltop.cdi  (self-boot disc image, burn with ImgBurn/
#                                    DiscJuggler, or run in an emulator)
#
# NOTE (runtime): the engine loads its assets from "assets/..." disk paths.
# The romdisk packs them under /rd/assets; wiring the engine's asset paths to
# the console file systems (romdisk/DFS) is follow-up porting work.

cd "$(dirname "$0")"

if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker not found. Install OrbStack (https://orbstack.dev) or Docker."
    exit 1
fi

# Generate the baked asset data (include_auto/src_auto + assets mirror)
.venv/bin/python3 scripts/generate_assets.py

# Toolchain image: SH4 toolchain + KOS + kos-ports + raylib for Dreamcast
docker build -q -t ltop/dreamcast -f scripts/console/dreamcast.Dockerfile scripts/console >/dev/null

# Compile + link + package inside the container
mkdir -p build/dreamcast/romdisk
rm -rf build/dreamcast/romdisk/assets
cp -R assets build/dreamcast/romdisk/assets
docker run --rm -v "$PWD":/src -w /src/build/dreamcast ltop/dreamcast bash -c '
    set -e
    source $KOS_BASE/environ.sh
    # -B: full rebuild each run; the engine is small and the KOS rules here do
    # not track header dependencies, so stale objects are worse than a slow link
    make -B -f /src/scripts/console/dreamcast.Makefile
    mkdcdisc --allow-overwrite -N -n "LTOP ENGINE" -a "ltop-engine" -e ltop.elf -o ltop.cdi
'

echo "Done. build/dreamcast/ltop.elf (dcload) and build/dreamcast/ltop.cdi (disc image)."

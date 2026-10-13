#!/bin/bash
set -e

# Build the game for the web with Emscripten + raylib.
# Requirements: emsdk on PATH (emcc / emmake). Install from https://emscripten.org
# or:  brew install emscripten
#
# Output: build/web/ - a static site (index.html, wasm, and the resources
# packaged inside). Host it over HTTP, e.g.:
#     cd build/web && python3 -m http.server
# (Browsers will not run wasm builds from file:// URLs.)

cd "$(dirname "$0")"

RAYLIB_TAG="${RAYLIB_TAG:-6.0}"
OUT="build/web"

if ! command -v emcc >/dev/null 2>&1 || ! command -v emmake >/dev/null 2>&1; then
    echo "error: Emscripten not found. Install emsdk and source emsdk_env.sh, or: brew install emscripten"
    exit 1
fi

# Locate raylib source. Set RAYLIB_SRC to use an existing checkout.
if [ -z "$RAYLIB_SRC" ]; then
    for candidate in vendor/raylib ../raylib "$HOME/raylib"; do
        if [ -d "$candidate/src/Makefile" ] || [ -f "$candidate/src/Makefile" ]; then
            RAYLIB_SRC="$candidate"
            break
        fi
    done
fi
if [ -z "$RAYLIB_SRC" ]; then
    echo "raylib source not found; downloading tag $RAYLIB_TAG..."
    mkdir -p build/vendor
    curl -L -o build/vendor/raylib.zip "https://github.com/raysan5/raylib/archive/refs/tags/$RAYLIB_TAG.zip"
    unzip -qo build/vendor/raylib.zip -d build/vendor
    RAYLIB_SRC="build/vendor/raylib-$RAYLIB_TAG"
fi
RAYLIB_SRC="$(cd "$RAYLIB_SRC" && pwd)"
echo "raylib source: $RAYLIB_SRC"

# Compile the raylib static library for the web
RAYLIB_LIB="$RAYLIB_SRC/src/libraylib.web.a"
( cd "$RAYLIB_SRC/src" && emmake make PLATFORM=PLATFORM_WEB -B )
if [ ! -f "$RAYLIB_LIB" ]; then
    echo "error: $RAYLIB_LIB was not produced"
    exit 1
fi

# Generate the baked asset data and link the game
.venv/bin/python3 scripts/generate_assets.py

# Game-local C++ (resources/code/*.cpp) compiles into the binary with the
# engine — the layer-4 seam (see NOTES.md "Custom behaviors").
GAME_CODE=""
for file in resources/code/*.cpp; do
    if [ -e "$file" ]; then GAME_CODE="$GAME_CODE $file"; fi
done

mkdir -p "$OUT"
em++ src/*.cpp src_auto/*.cpp $GAME_CODE "$RAYLIB_LIB" \
    -I"$RAYLIB_SRC/src" -Iinclude -Iinclude_auto \
    -DPLATFORM_WEB -O2 \
    -s USE_GLFW=3 -s ALLOW_MEMORY_GROWTH=1 -s ASYNCIFY \
    --shell-file scripts/web_shell.html \
    --preload-file assets \
    -o "$OUT/index.html"

echo "Done. Host it with, e.g.:  (cd $OUT && python3 -m http.server)"

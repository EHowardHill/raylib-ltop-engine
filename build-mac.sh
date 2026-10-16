#!/bin/bash
set -e
cd "$(dirname "$0")"

# Generate the header files and source files needed to compile the game
# All generated files will be placed in either include_auto or src_auto
.venv/bin/python3 scripts/generate_assets.py

# Game-local C++ (resources/code/*.cpp) compiles into the binary with the
# engine — this is the layer-4 seam (see NOTES.md "Custom behaviors").
GAME_CODE=""
for file in resources/code/*.cpp; do
    if [ -e "$file" ]; then GAME_CODE="$GAME_CODE $file"; fi
done

# All files from include, include_auto, src, and src_auto must be considered
clang++ src/*.cpp src_auto/*.cpp $GAME_CODE -Iinclude -Iinclude_auto -o main $(pkg-config --libs --cflags raylib)

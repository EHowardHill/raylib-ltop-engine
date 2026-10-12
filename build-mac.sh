#!/bin/bash

# Generate the header files and source files needed to compile the game
# All generated files will be placed in either include_auto or src_auto
.venv/bin/python3 scripts/generate_assets.py

# All files from include, include_auto, src, and src_auto must be considered
clang++ src/*.cpp src_auto/*.cpp -Iinclude -Iinclude_auto -o main $(pkg-config --libs --cflags raylib)

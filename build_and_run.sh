#!/bin/bash
# Quick build + run on the cycle-accurate backend (the default play path).
# No extras.c, no legacy patches: raw ROM on accurate hardware timing.
# A bare run opens a window; scripts pass the host's headless-implying
# flags (--frames/--input/--screenshot) instead.
set -e

cd "$(dirname "$0")"

ROM="roms/Super Dodge Ball (USA).nes"

echo "=== Building (cycle backend) ==="
# Vendored SDL2 ships Windows libs only: point the windowed build at system SDL2.
cmake -S external/nesrecomp/runner/cyc/project -B build-cycle \
  -DNESRECOMP_ROM="$PWD/$ROM" \
  -DNESRECOMP_GAME_CONFIG="$PWD/src/game.toml" \
  -DNESRECOMP_HEADLESS=OFF \
  -DSDL2_DIR=/usr/lib/x86_64-linux-gnu/cmake/SDL2
cmake --build build-cycle -j$(nproc)

echo "=== Running (cycle backend) ==="
./build-cycle/nes_game "$ROM" "$@"

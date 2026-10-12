#!/bin/bash
# Quick build + run on the cycle-accurate backend (the default play path).
# No extras.c, no legacy patches: raw ROM on accurate hardware timing.
# A bare run opens a window; scripts pass the host's headless-implying
# flags (--frames/--input/--screenshot) instead.
set -e

cd "$(dirname "$0")"

ROM="roms/Super Dodge Ball (USA).nes"

# recomp-ui: the pre-boot launcher + in-game menu. Set to OFF to build the
# window without them (NESRECOMP_RECOMP_UI=OFF ./build_and_run.sh).
RECOMP_UI="${NESRECOMP_RECOMP_UI:-$PWD/external/recomp-ui}"

echo "=== Building (cycle backend) ==="
# Vendored SDL2 ships Windows libs only: point the windowed build at system SDL2.
# RECOMP_UI is passed explicitly because project.cmake's auto-detect looks in
# ${CMAKE_SOURCE_DIR}/external/recomp-ui is passed explicitly (project.cmake's
# auto-detect only looks in ${CMAKE_SOURCE_DIR}/recomp-ui) -- being explicit
# keeps the build from depending on that coincidence.
#
# The project's own CMakeLists.txt (not the submodule's runner/cyc/project one)
# is configured here: it declares CXX and passes MODS / GAME_ID / HOST_EXTRAS,
# which the stock project file cannot express. The submodule stays pristine.
cmake -S . -B build-cycle \
  -DNESRECOMP_ROM="$PWD/$ROM" \
  -DNESRECOMP_GAME_CONFIG="$PWD/src/game.toml" \
  -DNESRECOMP_HEADLESS=OFF \
  -DNESRECOMP_RECOMP_UI="$RECOMP_UI" \
  -DSDL2_DIR=/usr/lib/x86_64-linux-gnu/cmake/SDL2
cmake --build build-cycle -j$(nproc)

echo "=== Running (cycle backend) ==="
./build-cycle/nes_game "$ROM" "$@"

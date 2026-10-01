#!/bin/bash
set -e

cd "$(dirname "$0")"

echo "=== Building ==="
# The cmake regen custom command does not reliably retrigger after
# game.toml/extras.c edits: regen manually when inputs are newer.
ROM="roms/Super Dodge Ball (USA).nes"
if [ ! -f build/super_dodgeball ] || [ src/game.toml -nt build/super_dodgeball ] || [ src/extras.c -nt build/super_dodgeball ]; then
  echo "--- Regenerating (stale inputs) ---"
  ./external/nesrecomp/recompiler/build/NESRecomp "$ROM" --game src/game.toml --output-prefix "Super_Dodge_Ball_(USA)"
fi
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

echo "=== Running ==="
./build/super_dodgeball "roms/Super Dodge Ball (USA).nes"

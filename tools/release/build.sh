#!/usr/bin/env bash
# Build a release binary for Linux or macOS from the repository root.
#
# The release artifact carries NO ROM: the launcher asks the player for their
# own copy at first launch. The ROM is needed HERE, because the build
# recompiles the cartridge's code into the executable.
#
# Usage:
#   tools/release/build.sh [--rom PATH] [--build-dir DIR] [--jobs N]
#                           [--sdl2-dir DIR] [--revision REV] [--version VER]
#
# Environment:
#   SDL2_DIR   path to SDL2's cmake package dir (the system one on Linux,
#              a locally built one on macOS). Auto-detected when unset.
#   NESRECOMP_ROM, NESRECOMP_GAME_CONFIG, NESRECOMP_RECOMP_UI
#              override the usual locations.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

ROM="${NESRECOMP_ROM:-${ROOT}/roms/Super Dodge Ball (USA).nes}"
GAME_CONFIG="${NESRECOMP_GAME_CONFIG:-${ROOT}/src/game.toml}"
RECOMP_UI="${NESRECOMP_RECOMP_UI:-${ROOT}/external/recomp-ui}"
BUILD_DIR="${ROOT}/build-release"
JOBS="$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu || echo 4)"
VERSION=""
REVISION=""
# The shipped executable's name. The CMake target stays "nes_game" (the
# engine's convention, and what this project's own tools point at). One
# file holds the name, so the build, the packager and the Flatpak manifest
# cannot drift apart.
EXE_NAME="$(tr -d '[:space:]' < "${SCRIPT_DIR}/exe.name")"

while [ $# -gt 0 ]; do
    case "$1" in
        --rom)       ROM="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --jobs)      JOBS="$2"; shift 2 ;;
        --sdl2-dir)  SDL2_DIR="$2"; export SDL2_DIR; shift 2 ;;
        --version)   VERSION="$2"; shift 2 ;;
        --revision)  REVISION="$2"; shift 2 ;;
        -h|--help)   sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

cd "${ROOT}"

if [ ! -f "${ROM}" ]; then
    cat >&2 <<EOF
error: no ROM at
  ${ROM}

The release build recompiles the cartridge's code, so it needs the ROM at
build time. The published archive does NOT contain it -- the launcher asks
the player for their own copy at first launch.

Drop a legally obtained copy at roms/Super Dodge Ball (USA).nes, or pass
--rom PATH.
EOF
    exit 2
fi

if [ ! -f "${RECOMP_UI}/recomp_ui.cmake" ]; then
    echo "error: recomp-ui checkout missing at ${RECOMP_UI}" >&2
    echo "       run: git submodule update --init external/recomp-ui" >&2
    exit 2
fi

# SDL2. The engine vendors a Windows-x64 build of SDL2 2.30.0 and puts its
# cmake dir on CMAKE_PREFIX_PATH, which would win the find_package(SDL2
# CONFIG) search on every platform -- and that config points at SDL2.dll and
# SDL2.lib, so a Linux or macOS link would fail. Name the platform's own
# SDL2 explicitly.
if [ -z "${SDL2_DIR:-}" ]; then
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2; then
        prefix="$(pkg-config --variable=prefix sdl2)"
        for candidate in "${prefix}/lib/cmake/SDL2" "${prefix}/lib/cmake/SDL2" \
                         "${prefix}/lib/x86_64-linux-gnu/cmake/SDL2" \
                         "${prefix}/lib/aarch64-linux-gnu/cmake/SDL2"; do
            if [ -f "${candidate}/sdl2-config.cmake" ]; then SDL2_DIR="${candidate}"; break; fi
        done
    fi
fi
if [ -z "${SDL2_DIR:-}" ] || [ ! -f "${SDL2_DIR}/sdl2-config.cmake" ]; then
    cat >&2 <<EOF
error: no usable SDL2 found.

  SDL2_DIR is unset and no system SDL2 was detected.

The engine's vendored SDL2 is a Windows-x64 binary and cannot be linked on
Linux or macOS. Install the development package:

  Debian/Ubuntu  sudo apt-get install libsdl2-dev
  Fedora         sudo dnf install SDL2-devel
  Arch           sudo pacman -S sdl2
  macOS          brew install sdl2   (or build it from source and pass --sdl2-dir)

or point SDL2_DIR at a cmake package dir that has sdl2-config.cmake.
EOF
    exit 2
fi
echo "SDL2: ${SDL2_DIR}"

# The launcher's hero art (the panel on the left of the game card). A local
# art/boxart.tga wins over the shipped one, so a player who has a scan of the
# retail box can build with it without putting that file in the repository.
BOXART="${ROOT}/art/boxart.tga"
if [ ! -f "${BOXART}" ]; then
    BOXART="${ROOT}/docs/assets/boxart.tga"
fi
[ -f "${BOXART}" ] || { echo "error: no launcher box art at ${BOXART}" >&2; exit 2; }
echo "box art: ${BOXART}"

if [ -z "${VERSION}" ]; then
    VERSION="$(head -n 1 "${ROOT}/VERSION" 2>/dev/null | tr -d '[:space:]')"
fi
VERSION="${VERSION:-development}"

if [ -z "${REVISION}" ]; then
    REVISION="$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
fi

echo "=== Configuring release build ==="
echo "    version  ${VERSION}"
echo "    revision ${REVISION}"
echo "    rom      ${ROM}"
echo "    build    ${BUILD_DIR}"

cmake -S "${ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNESRECOMP_ROM="${ROM}" \
    -DNESRECOMP_GAME_CONFIG="${GAME_CONFIG}" \
    -DNESRECOMP_HEADLESS=OFF \
    -DNESRECOMP_RECOMP_UI="${RECOMP_UI}" \
    -DNESRECOMP_GAME_VERSION="${VERSION}" \
    -DNESRECOMP_BUILD_REVISION="${REVISION}" \
    -DNESRECOMP_EXE_NAME="${EXE_NAME}" \
    -DSDL2_DIR="${SDL2_DIR}" \
    -DNESRECOMP_BOXART="${ROOT}/docs/assets/boxart.tga"

echo "=== Building ==="
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"

echo ""
echo "Release binary: ${BUILD_DIR}/${EXE_NAME}"
echo "Package it with: tools/release/package.py --build-dir ${BUILD_DIR} --platform $(uname -s | tr '[:upper:]' '[:lower:]')"

#!/usr/bin/env bash
# Build the Windows release binary by cross-compiling from Linux.
#
# The release artifact carries NO ROM: the launcher asks the player for their
# own copy at first launch. The ROM is needed HERE, because the build
# recompiles the cartridge's code into the executable -- which is the whole
# point of this route. The ROM never leaves the machine, and no CI secret
# has to hold it.
#
# Usage:
#   tools/release/build-mingw.sh [--rom PATH] [--build-dir DIR] [--jobs N]
#                                [--version VER] [--revision REV]
#
#   --build-dir DIR   where the Windows build tree goes
#                     (default: build-release/mingw)
#
# Package it with:
#   tools/release/package.py --build-dir build-release/mingw --platform windows
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

ROM="${NESRECOMP_ROM:-${ROOT}/roms/Super Dodge Ball (USA).nes}"
GAME_CONFIG="${NESRECOMP_GAME_CONFIG:-${ROOT}/src/game.toml}"
RECOMP_UI="${NESRECOMP_RECOMP_UI:-${ROOT}/external/recomp-ui}"
BUILD_DIR="${ROOT}/build-release/mingw"
HOST_RECOMPILER="${ROOT}/build-release/host-recompiler/NESRecomp"
JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4)"
VERSION=""
REVISION=""
# The shipped executable's name. The CMake target stays "nes_game" (the
# engine's convention, and what this project's own tools point at). One file
# holds the name, so the build, the packager and the Flatpak manifest cannot
# drift apart.
EXE_NAME="$(tr -d '[:space:]' < "${SCRIPT_DIR}/exe.name")"

while [ $# -gt 0 ]; do
    case "$1" in
        --rom)       ROM="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --jobs)      JOBS="$2"; shift 2 ;;
        --version)   VERSION="$2"; shift 2 ;;
        --revision)  REVISION="$2"; shift 2 ;;
        -h|--help)   sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

cd "${ROOT}"

# ---------------------------------------------------------------- the tools
if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
    cat >&2 <<EOF
error: no mingw-w64 cross compiler.

  sudo apt install g++-mingw-w64-x86-64

A cross build is the ONLY Windows route in this project. It keeps the
ROM on this machine and needs no secret at all -- one GitHub Actions
secret cannot hold this ROM (349,548 bytes of base64 against a 48 KB
cap; see docs/plan.md item 21).
EOF
    exit 2
fi
echo "cross compiler: $(x86_64-w64-mingw32-g++ --version | head -n 1)"

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

# ------------------------------------------------------------------ the SDL2
# The engine vendors an MSVC SDL2 and puts its cmake dir on
# CMAKE_PREFIX_PATH, which find_package(SDL2 CONFIG) would win with. Name
# the mingw one explicitly, or the link fails on an MSVC import library.
SDL2_PREFIX="$("${SCRIPT_DIR}/fetch_mingw_sdl2.sh" | tail -n 1 | sed 's/^mingw SDL2: //')"
SDL2_DIR="${SDL2_PREFIX}/lib/cmake/SDL2"
[ -f "${SDL2_DIR}/sdl2-config.cmake" ] || {
    echo "error: no mingw SDL2 at ${SDL2_DIR}" >&2; exit 2; }
echo "SDL2: ${SDL2_DIR}"

# ------------------------------------------------------- the host recompiler
# The code generator is a HOST program: it runs here, on Linux, and emits C.
# The engine refuses a cross build without one, because its own bootstrap
# would build a Windows recompiler and then try to run it.
if [ ! -x "${HOST_RECOMPILER}" ]; then
    echo "=== Building the host recompiler (Linux) ==="
    cmake -S "${ROOT}/external/nesrecomp/recompiler" \
          -B "$(dirname "${HOST_RECOMPILER}")" \
          -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$(dirname "${HOST_RECOMPILER}")" --parallel "${JOBS}" >/dev/null
fi
[ -x "${HOST_RECOMPILER}" ] || {
    echo "error: the host recompiler is missing at ${HOST_RECOMPILER}" >&2; exit 2; }
echo "host recompiler: ${HOST_RECOMPILER}"

# --------------------------------------------------------------- the version
if [ -z "${VERSION}" ]; then
    VERSION="$(head -n 1 "${ROOT}/VERSION" 2>/dev/null | tr -d '[:space:]')"
fi
VERSION="${VERSION:-development}"

if [ -z "${REVISION}" ]; then
    REVISION="$(git -C "${ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
fi

echo "=== Configuring the Windows build ==="
echo "    version  ${VERSION}"
echo "    revision ${REVISION}"
echo "    rom      ${ROM}"
echo "    build    ${BUILD_DIR}"

cmake -S "${ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${SCRIPT_DIR}/mingw-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNESRECOMP_ROM="${ROM}" \
    -DNESRECOMP_GAME_CONFIG="${GAME_CONFIG}" \
    -DNESRECOMP_HEADLESS=OFF \
    -DNESRECOMP_RECOMP_UI="${RECOMP_UI}" \
    -DNESRECOMP_HOST_COMPILER="${HOST_RECOMPILER}" \
    -DNESRECOMP_GAME_VERSION="${VERSION}" \
    -DNESRECOMP_BUILD_REVISION="${REVISION}" \
    -DNESRECOMP_EXE_NAME="${EXE_NAME}" \
    -DSDL2_DIR="${SDL2_DIR}" \
    -DNESRECOMP_BOXART="${ROOT}/docs/assets/boxart.tga"

# The guard the toolchain file cannot make for itself: the engine's vendored
# MSVC SDL2 is on CMAKE_PREFIX_PATH, and a build that silently linked it
# would produce an exe that cannot run. Check the cache.
CACHED_SDL2="$(sed -n 's/^SDL2_DIR:[^=]*=//p' "${BUILD_DIR}/CMakeCache.txt" | head -n 1)"
if [ "${CACHED_SDL2}" != "${SDL2_DIR}" ]; then
    echo "error: SDL2_DIR in the cache is '${CACHED_SDL2}', not the mingw package." >&2
    echo "       A build against the engine's vendored MSVC SDL2 links an" >&2
    echo "       MSVC import library and produces an exe that will not run." >&2
    exit 2
fi
echo "    SDL2_DIR  ${CACHED_SDL2} (mingw)"

echo "=== Building ==="
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"

EXE="${BUILD_DIR}/${EXE_NAME}.exe"
[ -f "${EXE}" ] || { echo "error: no ${EXE} after the build" >&2; exit 2; }

echo ""
echo "Windows binary: ${EXE}"
echo "Package it with: tools/release/package.py --build-dir ${BUILD_DIR} --platform windows"

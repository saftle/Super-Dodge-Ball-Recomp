#!/usr/bin/env bash
# Fetch and unpack the mingw-w64 build of SDL2, pinned and checksummed.
#
# The engine vendors an MSVC build of SDL2 2.30.0 and puts its cmake dir on
# CMAKE_PREFIX_PATH, which find_package(SDL2 CONFIG) searches. That is
# useless for a mingw build: its import library is an MSVC .lib. So the
# cross build needs its own SDL2 -- the official mingw development package,
# pinned by URL and by SHA-256 so a changed upstream artifact cannot slip in
# silently.
#
# The same posture as tools/release/appimage/build_appimage.sh, which pins
# linuxdeploy and appimagetool the same way.
#
# Usage:
#   tools/release/fetch_mingw_sdl2.sh [--dir DIR] [--print-prefix]
#
#   --dir DIR         where to keep the download and the unpacked tree
#                     (default: build-release/mingw-tools)
#   --print-prefix    print the mingw prefix and exit, without fetching
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# A versioned release, not a moving target.
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-2.30.0/SDL2-devel-2.30.0-mingw.tar.gz"
SDL2_SHA="d44bd8cd869219f43634a898d7735d0c92d4b2cca129f709dc8b98402b36d5f3"

TOOLS_DIR="${ROOT}/build-release/mingw-tools"
PRINT_PREFIX=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dir)          TOOLS_DIR="$2"; shift 2 ;;
        --print-prefix) PRINT_PREFIX=1; shift ;;
        -h|--help)      sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# The unpacked package has one prefix per target; this is the 64-bit one.
PREFIX="${TOOLS_DIR}/SDL2-2.30.0/x86_64-w64-mingw32"

if [ "${PRINT_PREFIX}" = "1" ]; then
    echo "${PREFIX}"
    exit 0
fi

mkdir -p "${TOOLS_DIR}"
ARCHIVE="${TOOLS_DIR}/SDL2-devel-2.30.0-mingw.tar.gz"

# The package is unpacked rather than downloaded in place: a stale tree from
# an older package would otherwise satisfy the "is it there" test.
if [ ! -f "${PREFIX}/lib/cmake/SDL2/sdl2-config.cmake" ]; then
    if [ ! -f "${ARCHIVE}" ] || \
       [ "$(sha256sum "${ARCHIVE}" | awk '{print $1}')" != "${SDL2_SHA}" ]; then
        echo "      fetching SDL2-devel-2.30.0-mingw.tar.gz"
        rm -f "${ARCHIVE}"
        curl -fL --retry 3 "${SDL2_URL}" -o "${ARCHIVE}.tmp"
        printf '%s  %s\n' "${SDL2_SHA}" "${ARCHIVE}.tmp" | sha256sum -c - >/dev/null
        mv "${ARCHIVE}.tmp" "${ARCHIVE}"
    else
        echo "      SDL2 archive already fetched and verified"
    fi
    echo "      unpacking"
    rm -rf "${TOOLS_DIR}/SDL2-2.30.0"
    tar xzf "${ARCHIVE}" -C "${TOOLS_DIR}"
fi

# The three things the build needs, checked rather than assumed: the cmake
# package (find_package), the import library (the link) and the runtime
# (beside the exe, or the game does not start).
for required in \
    "lib/cmake/SDL2/sdl2-config.cmake" \
    "lib/libSDL2.dll.a" \
    "bin/SDL2.dll"; do
    if [ ! -f "${PREFIX}/${required}" ]; then
        echo "error: the mingw SDL2 package is missing ${required}" >&2
        echo "       expected it at ${PREFIX}" >&2
        exit 2
    fi
done

echo "mingw SDL2: ${PREFIX}"

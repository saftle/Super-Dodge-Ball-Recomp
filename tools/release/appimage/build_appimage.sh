#!/usr/bin/env bash
# Build the Linux AppImage from a finished release build.
#
# The AppImage is a read-only squashfs. That matters here: the engine writes
# config.ini, the mod catalog's state.toml and the picked ROM path NEXT TO
# ITS EXECUTABLE, so a game run from inside the payload cannot start -- it
# exits with "mods: cannot write mod state". The AppRun therefore runs the
# game from a writable copy beside the .AppImage. See AppRun.in, and
# test_appimage_layout.sh, which measures that rather than assuming it.
#
# Usage:
#   tools/release/appimage/build_appimage.sh [--build-dir DIR] [--version V]
#                                            [--out DIR] [--jobs N] [--no-package]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

# Versioned releases, not "continuous": a continuous release is rebuilt
# upstream and its checksum stops matching, which breaks the release build
# for a reason that has nothing to do with the game.
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
APPIMAGETOOL_SHA="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"

BUILD_DIR="${ROOT}/build-release"
# The shipped executable's name (the CMake target stays "nes_game"). One
# file holds it, so the build and the packaging cannot drift apart.
EXE_NAME="$(tr -d '[:space:]' < "${SCRIPT_DIR}/../exe.name")"
OUT="${ROOT}/release"
VERSION=""
JOBS="$(nproc 2>/dev/null || echo 4)"
DO_PACKAGE=1

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)  BUILD_DIR="$2"; shift 2 ;;
        --out)        OUT="$2"; shift 2 ;;
        --version)    VERSION="$2"; shift 2 ;;
        --jobs)       JOBS="$2"; shift 2 ;;
        --no-package) DO_PACKAGE=0; shift ;;
        -h|--help)    sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

cd "${ROOT}"
[ -d "${BUILD_DIR}" ] || BUILD_DIR="${ROOT}/$(basename "${BUILD_DIR}")"

if [ -z "${VERSION}" ]; then
    VERSION="$(head -n 1 "${ROOT}/VERSION" 2>/dev/null | tr -d '[:space:]')"
fi
VERSION="${VERSION:-development}"

BIN="${BUILD_DIR}/${EXE_NAME}"
[ -f "${BIN}" ] || { echo "error: no ${BIN}. Run tools/release/build.sh first." >&2; exit 2; }
[ -d "${BUILD_DIR}/assets" ] || { echo "error: no launcher assets in ${BUILD_DIR}" >&2; exit 2; }

TOOLS_DIR="${BUILD_DIR}/appimage-tools"
mkdir -p "${TOOLS_DIR}"
fetch_tool() {
    local url="$1" sha="$2" dest="$3"
    if [ ! -f "${dest}" ] || [ "$(sha256sum "${dest}" | awk '{print $1}')" != "${sha}" ]; then
        echo "      fetching $(basename "${dest}")"
        curl -fL --retry 3 "${url}" -o "${dest}.tmp"
        printf '%s  %s\n' "${sha}" "${dest}.tmp" | sha256sum -c - >/dev/null
        mv "${dest}.tmp" "${dest}"
    fi
    chmod 0755 "${dest}"
}
LINUXDEPLOY_BIN="${TOOLS_DIR}/linuxdeploy-x86_64.AppImage"
APPIMAGETOOL_BIN="${TOOLS_DIR}/appimagetool-x86_64.AppImage"
fetch_tool "${LINUXDEPLOY_URL}" "${LINUXDEPLOY_SHA}" "${LINUXDEPLOY_BIN}"
fetch_tool "${APPIMAGETOOL_URL}" "${APPIMAGETOOL_SHA}" "${APPIMAGETOOL_BIN}"
# Both are AppImages themselves. --appimage-extract-and-run makes them work
# on a machine with no FUSE, which is most CI runners.
LINUXDEPLOY="${LINUXDEPLOY_BIN} --appimage-extract-and-run"
APPIMAGETOOL="${APPIMAGETOOL_BIN} --appimage-extract-and-run"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
APPDIR="${WORK}/AppDir"
mkdir -p "${APPDIR}"

ICON="${ROOT}/docs/assets/icon.png"
[ -f "${ICON}" ] || { echo "error: missing icon ${ICON}" >&2; exit 2; }

cat > "${WORK}/super-dodge-ball-recomp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Super Dodge Ball Recomp
Comment=Super Dodge Ball (USA), recompiled cycle-accurately
Exec=${EXE_NAME}
Icon=super-dodge-ball-recomp
Categories=Game;
Terminal=false
EOF

echo "=== Assembling the AppDir ==="
${LINUXDEPLOY} --appdir "${APPDIR}" --executable "${BIN}" \
    --desktop-file "${WORK}/super-dodge-ball-recomp.desktop" \
    --icon-file "${ICON}" \
    --icon-filename super-dodge-ball-recomp \
    || { echo "error: linuxdeploy failed" >&2; exit 1; }

# linuxdeploy copies the executable into usr/bin; the launcher, the mod
# catalog and the cheat file are resolved relative to it, so they go there
# too.
cp -r "${BUILD_DIR}/assets" "${APPDIR}/usr/bin/assets"
# Only the packages: state.toml is the PLAYER's (which features they have
# switched on) and is written at run time into the writable copy.
mkdir -p "${APPDIR}/usr/bin/mods"
cp -r "${BUILD_DIR}/mods/packages" "${APPDIR}/usr/bin/mods/packages"
cp -r "${BUILD_DIR}/cheats" "${APPDIR}/usr/bin/cheats"
mkdir -p "${APPDIR}/usr/bin/licenses"
for f in "${ROOT}/LICENSE.md" "${ROOT}/NOTICE.md" \
         "${ROOT}/external/nesrecomp/LICENSE" \
         "${ROOT}/external/nesrecomp/runner/external/SDL2/COPYING.txt" \
         "${ROOT}/external/recomp-ui/src/third_party/imgui/LICENSE.txt"; do
    [ -f "$f" ] || { echo "error: missing licence ${f}" >&2; exit 2; }
    cp "$f" "${APPDIR}/usr/bin/licenses/"
done

# The AppRun comes from the repository, not from here: it is the part that
# has to know the engine writes beside its executable.
#
# linuxdeploy leaves AppRun as a SYMLINK to the executable, so writing to it
# would write through the link and replace the game with this script.
rm -f "${APPDIR}/AppRun"
sed -e "s/@VERSION@/${VERSION}/g" -e "s/@EXE@/${EXE_NAME}/g" -e "s|@PAYLOAD@|usr/bin|g" \
    "${SCRIPT_DIR}/AppRun.in" > "${APPDIR}/AppRun"
chmod 0755 "${APPDIR}/AppRun"

echo "=== Layout and payload test ==="
bash "${SCRIPT_DIR}/test_appimage_layout.sh" "${APPDIR}" --rom "${ROOT}/roms/Super Dodge Ball (USA).nes"

if [ "${DO_PACKAGE}" = "0" ]; then
    echo "AppDir ready: ${APPDIR}"
    exit 0
fi

echo "=== Packaging the AppImage ==="
mkdir -p "${OUT}"
APP="${OUT}/super-dodge-ball-recomp-${VERSION}-linux-x86_64.AppImage"
rm -f "${APP}"
ARCH=x86_64 ${APPIMAGETOOL} "${APPDIR}" "${APP}"
chmod 0755 "${APP}"
echo "BUILT: ${APP} ($(du -h "${APP}" | cut -f1))"

#!/usr/bin/env bash
# Gate the Windows cross build.
#
# Four gates, in the order they can fail:
#
#   1. The executable is a PE32+ binary and links.
#   2. Its imports are right: the DLLs the runtime needs are there, and the
#      ones a published build must not need (libgcc_s_*, libstdc++-6.dll,
#      libwinpthread-1.dll) are not.
#   3. It RUNS -- under Wine, on this machine, which is the one check the
#      CI-with-MSVC route cannot give.
#   4. Its guest state is byte-identical to the Linux build's. This is the
#      gate that matters: it says the cross build executes the same machine.
#
# A Wine failure is not proof the exe is broken -- Wine is not Windows. But a
# pass is a real signal, and a failure has to be diagnosed, not dismissed.
#
# Usage:
#   tools/release/gate-mingw.sh [--build-dir DIR] [--linux-build-dir DIR]
#                                [--frames N] [--vector FILE] [--work DIR]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BUILD_DIR="${ROOT}/build-release/mingw"
LINUX_BUILD_DIR="${ROOT}/build-release"
FRAMES=3000
VECTOR="${ROOT}/tas/vectors/sdb_throw.input"
WORK=""
EXE_NAME="$(tr -d '[:space:]' < "${SCRIPT_DIR}/exe.name")"
ROM="${NESRECOMP_ROM:-${ROOT}/roms/Super Dodge Ball (USA).nes}"

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)       BUILD_DIR="$2"; shift 2 ;;
        --linux-build-dir) LINUX_BUILD_DIR="$2"; shift 2 ;;
        --frames)          FRAMES="$2"; shift 2 ;;
        --vector)          VECTOR="$2"; shift 2 ;;
        --work)            WORK="$2"; shift 2 ;;
        -h|--help)         sed -n '2,21p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

cd "${ROOT}"
[ -d "${BUILD_DIR}" ] || BUILD_DIR="${ROOT}/$(basename "${BUILD_DIR}")"
[ -d "${LINUX_BUILD_DIR}" ] || LINUX_BUILD_DIR="${ROOT}/$(basename "${LINUX_BUILD_DIR}")"
WORK="${WORK:-${ROOT}/tmp/mingw-gate}"
mkdir -p "${WORK}"

WIN_EXE="${BUILD_DIR}/${EXE_NAME}.exe"
LINUX_EXE="${LINUX_BUILD_DIR}/${EXE_NAME}"
FAILED=0
fail() { echo "  FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok:   $*"; }

echo "=============================================================="
echo " 1. the binary is a PE32+ executable"
echo "=============================================================="
if [ ! -f "${WIN_EXE}" ]; then
    echo "error: no ${WIN_EXE}. Run tools/release/build-mingw.sh first." >&2
    exit 2
fi
FILE_OUT="$(file -b "${WIN_EXE}")"
echo "  ${FILE_OUT}"
if echo "${FILE_OUT}" | grep -q "PE32+"; then
    pass "PE32+"
else
    fail "not a PE32+ binary: ${FILE_OUT}"
fi
if echo "${FILE_OUT}" | grep -q "x86-64"; then
    pass "x86-64"
else
    fail "not x86-64: ${FILE_OUT}"
fi

echo ""
echo "=============================================================="
echo " 2. the imports"
echo "=============================================================="
IMPORTS="$(x86_64-w64-mingw32-objdump -p "${WIN_EXE}" | sed -n 's/.*DLL Name: //p' | sort -u)"
echo "${IMPORTS}" | sed 's/^/  /'
# A PE's import table is case-insensitive, and the mingw linker writes
# "OPENGL32.dll" where the MSVC one writes "opengl32.dll". Compare folded.
for want in SDL2.dll opengl32.dll ws2_32.dll d2d1.dll dwrite.dll; do
    if echo "${IMPORTS}" | tr 'A-Z' 'a-z' | grep -qx "$(echo "${want}" | tr 'A-Z' 'a-z')"; then
        pass "imports ${want}"
    else
        fail "does not import ${want}"
    fi
done
# A published build must not need these beside it: they are the C++ runtime
# and the unwinder, and "it will not start" is the most common report for a
# Windows release.
for unwanted in libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    if echo "${IMPORTS}" | tr 'A-Z' 'a-z' | grep -qx "$(echo "${unwanted}" | tr 'A-Z' 'a-z')"; then
        fail "imports ${unwanted} -- the release must not need it beside the exe"
    else
        pass "does not need ${unwanted}"
    fi
done

echo ""
echo "=============================================================="
echo " 3. it runs"
echo "=============================================================="
# A 64-bit PE needs Wine's 64-bit loader. The "wine" wrapper on Debian and
# Ubuntu prefers the 32-bit one and then answers a 64-bit exe with "Bad
# EXE format", so the loader is named explicitly. And a prefix created by
# the 32-bit loader cannot run a 64-bit exe at all, so the prefix is a
# 64-bit one and is rebuilt if it is not.
WINE_BIN=""
for candidate in /usr/lib/wine/wine64 "$(command -v wine64 || true)" "$(command -v wine || true)"; do
    if [ -n "${candidate}" ] && [ -x "${candidate}" ]; then WINE_BIN="${candidate}"; break; fi
done
if [ -z "${WINE_BIN}" ]; then
    fail "no wine: the runtime gate needs it (sudo apt install wine wine64)"
else
    export WINEPREFIX="${WORK}/wineprefix64"
    export WINEDEBUG=-all
    # A headless run needs no display, but SDL would still try to open one.
    export SDL_VIDEODRIVER=dummy
    export SDL_AUDIODRIVER=dummy
    echo "  loader: ${WINE_BIN}"

    run_wine() {
        # A prefix made by the 32-bit loader is refused by the 64-bit one
        # ("is a 32-bit installation"), so it is rebuilt rather than
        # reported as a failure of the executable.
        if ! "${WINE_BIN}" "${WIN_EXE}" "$@" > "${RUN_OUT}" 2>&1; then
            if grep -q "32-bit installation" "${RUN_OUT}"; then
                rm -rf "${WINEPREFIX}"
                "${WINE_BIN}" "${WIN_EXE}" "$@" > "${RUN_OUT}" 2>&1
                return $?
            fi
            return 1
        fi
        return 0
    }

    RUN_OUT="${WORK}/wine-run.log"
    set +e
    run_wine "${ROM}" --frames 400 --input "${VECTOR}" --ram-init zeros
    WINE_STATUS=$?
    set -e
    tail -n 12 "${RUN_OUT}" | sed 's/^/  | /'
    if [ "${WINE_STATUS}" = "0" ]; then
        pass "wine run exited 0"
    else
        fail "wine run exited ${WINE_STATUS}"
    fi
    # The total cycle count, not the native one: "native_cycles=" also ends
    # in "cycles=", and a greedy match would pick that one up.
    WIN_CYCLES="$(grep -o '\bcycles=[0-9]*' "${RUN_OUT}" | head -n 1 | cut -d= -f2)"
    if [ -n "${WIN_CYCLES}" ]; then
        pass "wine run reported ${WIN_CYCLES} cycles"
    else
        fail "the wine run printed no cycle count"
    fi

    # The same schedule on the Linux build must cost the same cycles. A
    # different count would mean the two builds are not the same machine,
    # and no amount of comparing pictures would say so as plainly.
    if [ -f "${LINUX_EXE}" ]; then
        LINUX_RUN_OUT="${WORK}/linux-400.log"
        "${LINUX_EXE}" "${ROM}" --frames 400 --input "${VECTOR}" --ram-init zeros \
            > "${LINUX_RUN_OUT}" 2>&1 || true
        LINUX_CYCLES="$(grep -o '\bcycles=[0-9]*' "${LINUX_RUN_OUT}" | head -n 1 | cut -d= -f2)"
        if [ -z "${LINUX_CYCLES}" ]; then
            fail "the Linux build printed no cycle count to compare against"
        elif [ "${LINUX_CYCLES}" = "${WIN_CYCLES}" ]; then
            pass "the Linux build reports the same ${LINUX_CYCLES} cycles"
        else
            fail "the Linux build reports ${LINUX_CYCLES} cycles, the Windows build ${WIN_CYCLES}"
        fi
    else
        fail "no Linux build at ${LINUX_EXE} to compare the cycle count against"
    fi
fi

echo ""
echo "=============================================================="
echo " 4. the guest state matches the Linux build"
echo "=============================================================="
if [ ! -f "${LINUX_EXE}" ]; then
    fail "no Linux build at ${LINUX_EXE} to compare against"
elif [ -z "${WINE_BIN}" ]; then
    fail "no wine: the state comparison needs it"
else
    # Both builds run the SAME schedule from the SAME initial state. A
    # difference in the guest's memory is a difference in the machine, not
    # in the toolchain.
    LINUX_LOG="${WORK}/linux.bin"
    WIN_LOG="${WORK}/windows.bin"
    echo "  Linux  ${FRAMES} frames -> ${LINUX_LOG}"
    "${LINUX_EXE}" "${ROM}" --frames "${FRAMES}" --input "${VECTOR}" --ram-init zeros \
        --frame-log "${LINUX_LOG}" --frame-log-frames "0:$((FRAMES - 1))" \
        > "${WORK}/linux-run.log" 2>&1
    echo "  Windows ${FRAMES} frames -> ${WIN_LOG} (under wine)"
    RUN_OUT="${WORK}/wine-log-run.log"
    set +e
    run_wine "${ROM}" --frames "${FRAMES}" --input "${VECTOR}" --ram-init zeros \
        --frame-log "${WIN_LOG}" --frame-log-frames "0:$((FRAMES - 1))"
    WINE_LOG_STATUS=$?
    set -e
    if [ "${WINE_LOG_STATUS}" != "0" ]; then
        fail "the wine frame-log run exited ${WINE_LOG_STATUS}"
        tail -n 12 "${RUN_OUT}" | sed 's/^/  | /'
    elif [ ! -f "${WIN_LOG}" ]; then
        fail "the wine run wrote no frame log"
    else
        LINUX_SIZE="$(stat -c %s "${LINUX_LOG}")"
        WIN_SIZE="$(stat -c %s "${WIN_LOG}")"
        echo "  Linux  ${LINUX_SIZE} bytes"
        echo "  Windows ${WIN_SIZE} bytes"
        if [ "${LINUX_SIZE}" != "${WIN_SIZE}" ]; then
            fail "the two frame logs differ in size"
        elif cmp -s "${LINUX_LOG}" "${WIN_LOG}"; then
            pass "byte-identical over ${FRAMES} frames"
        else
            fail "the frame logs differ"
            cmp "${LINUX_LOG}" "${WIN_LOG}" 2>&1 | head -n 3 | sed 's/^/  | /'
        fi
    fi
fi

echo ""
echo "=============================================================="
echo " 5. the launcher opens"
echo "=============================================================="
# The launcher is the FIRST thing a player sees, and it is a different code
# path from the headless one: Direct2D, DirectWrite, OpenGL and SDL's
# window. A headless run touches none of it, so a binary that passes the
# first four gates can still show a player a black rectangle.
#
# It runs from a COPY of the build tree. The engine writes config.ini,
# rom.cfg and the mod catalog's state.toml next to its executable, and
# that state is the PLAYER's -- putting it in the build tree would leave
# a second run reading a state this gate wrote.
if [ -z "${WINE_BIN}" ]; then
    fail "no wine: the launcher gate needs it"
elif ! command -v Xvfb >/dev/null 2>&1 || ! command -v xwininfo >/dev/null 2>&1; then
    fail "the launcher gate needs Xvfb and xwininfo (sudo apt install xvfb x11-utils)"
else
    RUN_DIR="${WORK}/launcher-run"
    rm -rf "${RUN_DIR}"
    mkdir -p "${RUN_DIR}"
    cp "${WIN_EXE}" "${RUN_DIR}/"
    [ -f "${BUILD_DIR}/SDL2.dll" ] && cp "${BUILD_DIR}/SDL2.dll" "${RUN_DIR}/"
    [ -d "${BUILD_DIR}/assets" ] && cp -r "${BUILD_DIR}/assets" "${RUN_DIR}/"
    [ -d "${BUILD_DIR}/mods" ] && cp -r "${BUILD_DIR}/mods" "${RUN_DIR}/"
    [ -d "${BUILD_DIR}/cheats" ] && cp -r "${BUILD_DIR}/cheats" "${RUN_DIR}/"

    XVFB_DISPLAY=":97"
    Xvfb "${XVFB_DISPLAY}" -screen 0 1024x768x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 2
    # The headless gates above set SDL_VIDEODRIVER=dummy, and the launcher
    # cannot make a window on the dummy driver -- it says so, and stops.
    # It wants SDL's DEFAULT driver, which for a Windows build is "windows"
    # (Wine's win32 layer draws it on X11 underneath). Naming "x11" is
    # wrong: a Windows SDL2 has no x11 driver to name.
    ( cd "${RUN_DIR}" && env -u SDL_VIDEODRIVER DISPLAY="${XVFB_DISPLAY}" \
        "${WINE_BIN}" "${WIN_EXE}" "${ROM}" \
        > "${WORK}/launcher.log" 2>&1 ) &
    WINE_PID=$!
    # Poll for the window rather than sleeping a fixed time: the launcher
    # takes a moment, and how long depends on the machine.
    FOUND=""
    for _ in $(seq 1 30); do
        sleep 1
        if xwininfo -root -tree -display "${XVFB_DISPLAY}" 2>/dev/null | grep -q "Launcher"; then
            FOUND=yes; break
        fi
    done
    kill "${WINE_PID}" 2>/dev/null || true
    sleep 1
    kill "${XVFB_PID}" 2>/dev/null || true
    if [ -n "${FOUND}" ]; then
        pass "the launcher window opened"
        grep -i "launcher\|emoji" "${WORK}/launcher.log" 2>/dev/null | head -n 4 | sed 's/^/  | /'
    else
        fail "no launcher window appeared"
        tail -n 12 "${WORK}/launcher.log" 2>/dev/null | sed 's/^/  | /'
    fi
fi

echo ""
echo "=============================================================="
if [ "${FAILED}" = "0" ]; then
    echo " all gates passed"
    exit 0
fi
echo " a gate failed"
exit 1

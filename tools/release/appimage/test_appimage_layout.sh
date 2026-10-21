#!/bin/sh
# AppImage layout and payload test.
#
#   test_appimage_layout.sh /path/to/AppDir [--rom /path/to/ROM]
#
# The reference layout test in the Super Mario Bros. recomp checks that the
# AppRun seeds the mod catalog beside the AppImage and that the payload is
# not modified -- but it runs the game with "|| true" and never looks at the
# exit code. That misses the failure that matters: the engine writes
# config.ini, the mod catalog's state.toml and the picked ROM path NEXT TO
# ITS EXECUTABLE, so a game run from inside the read-only squashfs cannot
# start. It exits 2 with "mods: cannot write mod state".
#
# This test therefore asserts the positive: after a run, the settings file
# and the mod state exist in a WRITABLE directory, and the run did not report
# a failed write.
set -eu

APP_DIR=""
ROM=""
while [ $# -gt 0 ]; do
    case "$1" in
        --rom) ROM="$2"; shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) APP_DIR="$1"; shift ;;
    esac
done
[ -n "${APP_DIR}" ] || { echo "usage: $0 /path/to/AppDir [--rom ROM]" >&2; exit 2; }

appdir=$(CDPATH= cd -- "${APP_DIR}" && pwd)
EXE_NAME="$(tr -d '[:space:]' < "$(dirname "$0")/../exe.name")"
exe="${appdir}/usr/bin/${EXE_NAME}"

[ -x "${exe}" ] || { echo "missing game executable: ${exe}" >&2; exit 1; }
# It must be the ELF, not a script that replaced it: linuxdeploy leaves
# AppRun as a symlink to the executable, so writing AppRun without removing
# that link replaces the game with the script.
head -c 4 "${exe}" | grep -q ELF || {
    echo "the game executable is not an ELF binary" >&2; exit 1; }
[ -x "${appdir}/AppRun" ] || { echo "missing AppRun" >&2; exit 1; }
head -c 2 "${appdir}/AppRun" | grep -q '#!' || {
    echo "AppRun is not a script" >&2; exit 1; }
[ -d "${appdir}/usr/bin/assets" ] || { echo "missing launcher assets" >&2; exit 1; }
[ -d "${appdir}/usr/bin/mods/packages" ] || { echo "missing mod packages" >&2; exit 1; }
[ -d "${appdir}/usr/bin/cheats" ] || { echo "missing cheats" >&2; exit 1; }
[ -d "${appdir}/usr/bin/licenses" ] || { echo "missing licences" >&2; exit 1; }

# The release's own mod manifests, and nothing else.
required_mods='packages/super-dodge-ball.character-flicker/1.0.0/manifest.toml
packages/super-dodge-ball.ppu-overclock/1.0.0/manifest.toml
packages/super-dodge-ball.widescreen/1.0.0/manifest.toml'
actual_mods=$(cd "${appdir}/usr/bin/mods" && find . -type f -printf '%P\n' | sort)
expected_mods=$(printf '%s\n' "${required_mods}" | sort)
[ "${actual_mods}" = "${expected_mods}" ] || {
    echo "preloaded mod inventory differs from the pristine manifests" >&2
    printf 'expected:\n%s\nactual:\n%s\n' "${expected_mods}" "${actual_mods}" >&2
    exit 1; }

# No ROM and no player data in the payload.
if find "${appdir}" -type f \( -iname '*.nes' -o -iname '*.sav' -o -iname '*.state' \
    -o -iname '*.cycstate' -o -name 'state.toml' -o -name 'rom.cfg' \
    -o -name 'config.ini' -o -name 'keybinds.ini' \) | grep -q .; then
    echo "forbidden ROM or player data in the AppDir payload" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'chmod -R u+w "${appdir}" "${tmp}" 2>/dev/null || true; rm -rf "${tmp}"' EXIT HUP INT TERM

# The payload must be read-only while the game runs, or the test proves
# nothing: the whole point is that the payload cannot be written to.
before="${tmp}/before.sha"
after="${tmp}/after.sha"
(cd "${appdir}" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > "${before}"
chmod -R a-w "${appdir}"

state="${tmp}/state"
mkdir -p "${state}"

run_app() {
    # The run is EXPECTED to end in a non-zero status (the windowed host
    # runs until it is killed), so its status is captured rather than
    # aborting the test.
    code=0
    APPIMAGE="${state}/SuperDodgeballRecomp.AppImage" \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy NESRECOMP_NO_LAUNCHER=1 \
    timeout 20 "${appdir}/AppRun" ${ROM:+"${ROM}"} > "${tmp}/run.log" 2>&1 || code=$?
    echo "${code}" > "${tmp}/run.code"
}

run_app
code=$(cat "${tmp}/run.code")

# 1. The decisive check: the run must not have failed to write the settings
#    or the mod state. Both messages are what a read-only payload produces.
if grep -q "cannot write" "${tmp}/run.log"; then
    echo "the game could not write its own files -- it is running from the read-only payload" >&2
    sed -n '1,20p' "${tmp}/run.log" >&2
    exit 1
fi
if [ "${code}" = "2" ]; then
    echo "the game exited 2 (the engine's refusal to start)" >&2
    sed -n '1,20p' "${tmp}/run.log" >&2
    exit 1
fi

# 2. The positive check: the settings file and the mod state were written to
#    a writable directory. A game running from inside the payload writes
#    neither, and this is the assertion the reference test is missing.
runtime="${state}/.super-dodge-ball-recomp"
if [ ! -f "${runtime}/config.ini" ]; then
    echo "the run left no config.ini in a writable directory" >&2
    echo "runtime dir: ${runtime}" >&2
    ls -la "${state}" >&2 || true
    echo "--- the run's own output ---" >&2
    sed -n '1,40p' "${tmp}/run.log" >&2 || true
    exit 1
fi
[ -f "${runtime}/mods/state.toml" ] || {
    echo "the run left no mod state in a writable directory" >&2
    exit 1; }

# 3. The player's own state survives a refresh of the payload.
#
#    The mod STATE file is the game's own: the engine rewrites it at every
#    start (it commits the committed selection), so its content is not the
#    player's to keep. The player's own mod PACKAGES are, and so are the
#    launcher's settings.
mkdir -p "${runtime}/mods/packages/user.example/1.0.0"
printf 'user-owned\n' > "${runtime}/mods/packages/user.example/1.0.0/marker.txt"
printf 'user-state\n' > "${runtime}/mods/state.toml"
printf '[Display]\nWindowScale = 3\n' > "${runtime}/config.ini"
# Pretend the installed payload is an older version, so the next run
# refreshes it.
printf '0.0.0\n' > "${runtime}/.payload-version"
run_app
[ "$(cat "${runtime}/mods/packages/user.example/1.0.0/marker.txt")" = "user-owned" ] || {
    echo "AppRun overwrote a user-installed mod" >&2; exit 1; }
[ -f "${runtime}/mods/state.toml" ] || {
    echo "the refresh left no mod state" >&2; exit 1; }
grep -q "WindowScale = 3" "${runtime}/config.ini" || {
    echo "AppRun overwrote the user's settings" >&2; exit 1; }

# 4. The payload itself was not modified.
(cd "${appdir}" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > "${after}"
cmp -s "${before}" "${after}" || { echo "AppRun modified its read-only payload" >&2; exit 1; }

echo "AppImage layout test: PASS"

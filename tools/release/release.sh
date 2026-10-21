#!/usr/bin/env bash
# Publish a release, built here, from here.
#
# The release artifacts carry NO ROM: the launcher asks the player for their
# own copy at first launch. The ROM is needed HERE, because the build
# recompiles the cartridge's code into the executable -- so the build runs on
# the machine that owns the ROM, and the ROM never leaves it. There is no
# secret, no CI job that needs one, and no copy of the cartridge on anybody
# else's servers.
#
# This is one command that does all of it:
#
#     tools/release/release.sh              # build, package, verify
#     tools/release/release.sh --publish    # ... and publish the release
#
# What it does, in order, and what each step is for:
#
#   preflight   the tree is clean, the tag agrees with VERSION, the ROM is
#               the right ROM, the toolchain is installed
#   build       Linux, then the Windows cross build, then the AppImage and
#               the Flatpak
#   gate        the Windows build's five gates, before the ZIP is made
#   package     the tarball and the ZIP
#   verify      the artifacts are the ones this commit produces
#   notes       the release notes, with the checksums
#   publish     the tag, the release, and the assets
#
# The clean-tree and HEAD-unchanged checks are the point of the "verify"
# step: the build bakes the commit it came from into the binary, so an
# artifact built from a dirty tree names a commit that is not what you
# shipped. A release must not be made from a tree that is not the tag.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

BUILD_DIR="${ROOT}/build-release"
MINGW_BUILD_DIR="${BUILD_DIR}/mingw"
OUT="${ROOT}/release"
JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4)"
VERSION=""
TAG=""
PUBLISH=0
DO_FLATPAK=1
DO_GATE=1
NOTES="${OUT}/RELEASE-NOTES.md"

# The cartridge's identity, as the whole .nes file. The build recompiles
# this exact cartridge, so building a different one would produce an
# artifact that is not this project.
ROM_SHA1="75dad8b461864ddd396c75bdb68378bb2bc47d99"
ROM="${NESRECOMP_ROM:-${ROOT}/roms/Super Dodge Ball (USA).nes}"

EXE_NAME="$(tr -d '[:space:]' < "${SCRIPT_DIR}/exe.name")"

while [ $# -gt 0 ]; do
    case "$1" in
        --version)      VERSION="$2"; shift 2 ;;
        --tag)          TAG="$2"; shift 2 ;;
        --out)          OUT="$2"; shift 2 ;;
        --jobs)         JOBS="$2"; shift 2 ;;
        --publish)      PUBLISH=1; shift ;;
        --skip-flatpak) DO_FLATPAK=0; shift ;;
        --skip-gate)    DO_GATE=0; shift ;;
        -h|--help)      sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

cd "${ROOT}"
mkdir -p "${OUT}"

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
info() { printf '   %s\n' "$*"; }
die()  { printf '\nerror: %s\n' "$*" >&2; exit 2; }

# ============================================================== the preflight
say "Preflight"

command -v git >/dev/null 2>&1 || die "git is not installed"
command -v cmake >/dev/null 2>&1 || die "cmake is not installed"

if [ -z "${VERSION}" ]; then
    [ -f "${ROOT}/VERSION" ] || die "no VERSION file at the repository root"
    VERSION="$(head -n 1 "${ROOT}/VERSION" | tr -d '[:space:]')"
fi
[ -n "${VERSION}" ] || die "the version is empty"
if [ -z "${TAG}" ]; then
    TAG="v${VERSION}"
fi
info "version  ${VERSION}"
info "tag      ${TAG}"

# The tree must be clean, and must stay clean. The build bakes the commit
# into the binary, and a dirty tree means the binary is not the commit.
if [ -n "$(git status --porcelain)" ]; then
    echo "error: the working tree has changes." >&2
    git status --short >&2
    echo "       A release must be built from a clean tree: the build bakes" >&2
    echo "       the commit into the binary, and an artifact built from a" >&2
    echo "       dirty tree names a commit that is not what you shipped." >&2
    echo "       Commit or stash first." >&2
    exit 2
fi
HEAD_SHA="$(git rev-parse HEAD)"
HEAD_SHORT="$(git rev-parse --short HEAD)"
info "commit   ${HEAD_SHORT}"

# The tag, if it exists, has to be this commit.
if git rev-parse -q --verify "refs/tags/${TAG}" >/dev/null 2>&1; then
    TAGGED="$(git rev-list -n 1 "${TAG}")"
    [ "${TAGGED}" = "${HEAD_SHA}" ] || die "the tag ${TAG} already exists and points at $(git rev-parse --short "${TAGGED}"), not ${HEAD_SHORT}"
    info "tag      already exists, at this commit"
else
    info "tag      will be created at this commit"
fi

# The ROM. It is a build input and it never leaves this machine.
[ -f "${ROM}" ] || die "no ROM at ${ROM}
       Drop a legally obtained copy there, or set NESRECOMP_ROM."
ROM_SHA1_ACTUAL="$(sha1sum "${ROM}" | awk '{print $1}')"
[ "${ROM_SHA1_ACTUAL}" = "${ROM_SHA1}" ] || die "${ROM} is not the cartridge this project builds for.
       expected SHA-1 ${ROM_SHA1}
       got      SHA-1 ${ROM_SHA1_ACTUAL}"
info "rom      the right cartridge (SHA-1 ${ROM_SHA1_ACTUAL:0:12}...)"

command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1 || die "no mingw-w64 cross compiler.
       sudo apt install g++-mingw-w64-x86-64
       The Windows build is cross-compiled from here; there is no other route."

if [ "${PUBLISH}" = "1" ]; then
    command -v gh >/dev/null 2>&1 || die "gh is not installed (needed to publish)"
    gh auth status >/dev/null 2>&1 || die "gh is not authenticated. Run: gh auth login"
fi

# flatpak-builder, as a program or as the Builder flatpak.
FLATPAK_BUILDER=""
if [ "${DO_FLATPAK}" = "1" ]; then
    if command -v flatpak-builder >/dev/null 2>&1; then
        FLATPAK_BUILDER="flatpak-builder"
    elif flatpak list --user 2>/dev/null | grep -q "org.flatpak.Builder"; then
        # The Builder flatpak's command IS flatpak-builder, so the arguments
        # pass straight through -- do not put another "flatpak-builder" in
        # front of them, or it becomes a stray positional argument and
        # flatpak-builder reads the build directory as the manifest.
        FLATPAK_BUILDER="flatpak run org.flatpak.Builder"
    else
        die "no flatpak-builder, and the Builder flatpak is not installed.
       Either:  sudo apt install flatpak-builder
       Or:      flatpak install --user flathub org.flatpak.Builder
       Or skip the Flatpak:  --skip-flatpak"
    fi
    info "flatpak  ${FLATPAK_BUILDER}"
fi

# =================================================================== the build
say "Building the Linux artifacts"
"${SCRIPT_DIR}/build.sh" --build-dir "${BUILD_DIR}" --jobs "${JOBS}" \
    --version "${VERSION}" --revision "${HEAD_SHORT}" | tail -3

say "Building the Windows artifact (cross-compiled)"
"${SCRIPT_DIR}/build-mingw.sh" --build-dir "${MINGW_BUILD_DIR}" \
    --version "${VERSION}" --revision "${HEAD_SHORT}" | tail -3

if [ "${DO_GATE}" = "1" ]; then
    say "Gating the Windows build"
    "${SCRIPT_DIR}/gate-mingw.sh" --build-dir "${MINGW_BUILD_DIR}" \
        --linux-build-dir "${BUILD_DIR}" | tail -8
else
    printf '\n\033[1m== Gating the Windows build\033[0m\n   SKIPPED (--skip-gate).\n'
    printf '   The Windows artifact is NOT verified. Say so in the release notes.\n'
fi

say "Building the AppImage"
"${SCRIPT_DIR}/appimage/build_appimage.sh" --build-dir "${BUILD_DIR}" \
    --version "${VERSION}" --out "${OUT}" | tail -2

FLATPAK_BUNDLE=""
if [ "${DO_FLATPAK}" = "1" ]; then
    say "Building the Flatpak"
    ${FLATPAK_BUILDER} --user --install --force-clean --disable-cache \
        --repo="${ROOT}/tmp/flatpak-repo" "${ROOT}/tmp/flatpak-build" \
        "${ROOT}/tools/release/flatpak/org.saftle.SuperDodgeBallRecomp.yml" | tail -3
    FLATPAK_BUNDLE="${OUT}/super-dodge-ball-recomp-${VERSION}.flatpak"
    rm -f "${FLATPAK_BUNDLE}"
    flatpak build-bundle "${ROOT}/tmp/flatpak-repo" "${FLATPAK_BUNDLE}" \
        org.saftle.SuperDodgeBallRecomp
    info "built ${FLATPAK_BUNDLE}"
fi

# ================================================================ the package
say "Packaging"
python3 "${SCRIPT_DIR}/package.py" --build-dir "${BUILD_DIR}" --platform linux \
    --arch x86_64 --version "${VERSION}" --revision "${HEAD_SHORT}" --out "${OUT}" | tail -2
python3 "${SCRIPT_DIR}/package.py" --build-dir "${MINGW_BUILD_DIR}" --platform windows \
    --arch x64 --version "${VERSION}" --revision "${HEAD_SHORT}" --out "${OUT}" | tail -2

# ================================================================== the verify
say "Verifying"
[ "${HEAD_SHA}" = "$(git rev-parse HEAD)" ] || die "HEAD moved during the build -- the artifacts are not this commit"
[ -z "$(git status --porcelain)" ] || die "the working tree changed during the build -- the artifacts are not this commit"

ARTIFACTS=(
    "${OUT}/super-dodge-ball-recomp-${VERSION}-linux-x86_64.tar.gz"
    "${OUT}/super-dodge-ball-recomp-${VERSION}-linux-x86_64.AppImage"
    "${OUT}/super-dodge-ball-recomp-${VERSION}-windows-x64.zip"
)
if [ -n "${FLATPAK_BUNDLE}" ]; then
    ARTIFACTS+=("${FLATPAK_BUNDLE}")
fi
for artifact in "${ARTIFACTS[@]}"; do
    [ -f "${artifact}" ] || die "missing artifact: ${artifact}"
    info "$(basename "${artifact}")  $(du -h "${artifact}" | cut -f1)"
done

# The archives must not carry a ROM. package.py refuses to write one for
# the tarball and the ZIP; the AppImage and the Flatpak are assembled by
# other scripts, so they are checked here, by looking inside them.
EXTRACT="$(mktemp -d)"
trap 'rm -rf "${EXTRACT}"' EXIT
for artifact in "${ARTIFACTS[@]}"; do
    listing=""
    case "${artifact}" in
        *.tar.gz)
            listing="$(tar tzf "${artifact}")" ;;
        *.zip)
            listing="$(unzip -Z1 "${artifact}")" ;;
        *.AppImage)
            # --appimage-extract reads the payload without FUSE, so this
            # works on a machine with no FUSE (and on a CI runner).
            (cd "${EXTRACT}" && "${artifact}" --appimage-extract >/dev/null 2>&1) \
                || die "could not read the AppImage's payload"
            listing="$(cd "${EXTRACT}" && find . -type f)" ;;
        *.flatpak)
            # The bundle is made from the repository, and the repository is
            # made from these files, so this is the payload's content.
            [ -d "${ROOT}/tmp/flatpak-build/files" ] \
                || die "the Flatpak's payload directory is missing; the check would measure nothing"
            listing="$(cd "${ROOT}/tmp/flatpak-build/files" && find . -type f)" ;;
        *)
            continue ;;
    esac
    # A check that read nothing is not a check that passed.
    [ -n "${listing}" ] \
        || die "could not list $(basename "${artifact}"); the ROM check would measure nothing"
    if echo "${listing}" | grep -qiE '\.(nes|fds|gb|gbc|smc|sfc)$'; then
        die "$(basename "${artifact}") contains a ROM"
    fi
done
info "no ROM in any of the ${#ARTIFACTS[@]} artifacts"

# ================================================================== the notes
say "Release notes"
{
    echo "Super Dodge Ball Recomp ${VERSION}."
    echo
    echo "The game is a cycle-accurate recompilation of Super Dodge Ball (USA)."
    echo "**No ROM is included.** The launcher asks for your own copy at first launch."
    echo
    echo "| Artifact | Platform |"
    echo "|---|---|"
    for artifact in "${ARTIFACTS[@]}"; do
        name="$(basename "${artifact}")"
        case "${name}" in
            *.tar.gz)   echo "| ${name} | Linux x86-64 (tarball) |" ;;
            *.AppImage) echo "| ${name} | Linux x86-64 (AppImage) |" ;;
            *.zip)      echo "| ${name} | Windows x64 |" ;;
            *.flatpak)  echo "| ${name} | Linux x86-64 (Flatpak) |" ;;
        esac
    done
    if [ "${DO_GATE}" = "0" ]; then
        echo
        echo "> **The Windows build was not gated for this release.** Its"
        echo "> guest state was not compared against the Linux build's."
    fi
    echo
    echo "Checksums:"
    echo
    echo '```'
    for artifact in "${ARTIFACTS[@]}"; do
        (cd "${OUT}" && sha256sum "$(basename "${artifact}")")
    done
    echo '```'
} > "${NOTES}"
cat "${NOTES}"

# ================================================================== the publish
if [ "${PUBLISH}" != "1" ]; then
    say "Built and verified. Not published."
    echo "   Publish it with:"
    echo "     tools/release/release.sh --publish"
    echo
    echo "   Or, if you want to look at the notes first:"
    echo "     less ${NOTES}"
    exit 0
fi

say "Publishing"
if ! git rev-parse -q --verify "refs/tags/${TAG}" >/dev/null 2>&1; then
    git tag -a "${TAG}" -m "Super Dodge Ball Recomp ${VERSION}"
    git push origin "${TAG}"
    info "tagged and pushed ${TAG}"
else
    info "${TAG} already exists"
fi

if gh release view "${TAG}" >/dev/null 2>&1; then
    # The release exists: replace its assets and its notes rather than
    # deleting the release, which would take its URL and its discussion
    # with it.
    gh release upload "${TAG}" "${ARTIFACTS[@]}" --clobber
    gh release edit "${TAG}" --title "Super Dodge Ball Recomp ${VERSION}" --notes-file "${NOTES}"
    info "replaced the assets on the existing release"
else
    gh release create "${TAG}" "${ARTIFACTS[@]}" \
        --title "Super Dodge Ball Recomp ${VERSION}" --notes-file "${NOTES}"
    info "created the release"
fi

say "Published"
gh release view "${TAG}" --json name,tagName,url,assets \
    --jq '"  \(.tagName)  \(.name)\n  \(.url)\n" + (.assets[] | "  \(.name)  \(.size) bytes")'

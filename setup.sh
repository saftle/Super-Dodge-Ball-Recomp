#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${SCRIPT_DIR}/venv"
PYTHON_BIN="${VENV_DIR}/bin/python3"
NESASM_BIN="${VENV_DIR}/bin/nesasm"
MESENCE_DIR="${SCRIPT_DIR}/external/mesence"
MESEN_BIN="${SCRIPT_DIR}/external/mesence/Mesen"
FRAME_GEN_DIR="${SCRIPT_DIR}/tools/frame_gen"

echo "=== Super Dodgeball Recomp Setup ==="
echo ""

# ---- Build dependencies check ----
echo ""
echo "Checking build dependencies..."
DEPS_OK=1
if ! command -v g++ &>/dev/null; then echo "MISSING: g++ — required to build MesenCE"; DEPS_OK=0; fi
if ! command -v make &>/dev/null; then echo "MISSING: make — required to build MesenCE"; DEPS_OK=0; fi
if ! command -v sdl2-config &>/dev/null; then echo "MISSING: SDL2 dev — required to build MesenCE. Install: sudo apt-get install libsdl2-dev"; DEPS_OK=0; fi
if ! command -v dotnet &>/dev/null; then echo "NOTE: .NET SDK not found — needed only if building MesenCE from source. Download pre-built binary instead (see README.md)."; fi
if ! command -v xvfb-run &>/dev/null; then echo "MISSING: xvfb — required for headless Mesen. Install: sudo apt-get install xvfb"; DEPS_OK=0; fi
if [ "${DEPS_OK}" = "0" ]; then echo "" ; echo "Install missing dependencies and re-run setup.sh."; exit 1; fi

# ---- Virtual environment ----
if [ ! -d "${VENV_DIR}" ]; then
    echo "Creating virtual environment at ${VENV_DIR}..."
    python3 -m venv "${VENV_DIR}"
fi

echo "Installing Python dependencies (non-fatal — venv pip is broken on some machines)..."
if "${PYTHON_BIN}" -m pip install --upgrade pip 2>&1 | tail -1 \
    && "${PYTHON_BIN}" -m pip install -e "${SCRIPT_DIR}" 2>&1 | tail -1; then
    echo ""
    echo "Verifying nesasm..."
    "${NESASM_BIN}" asm --help 2>&1 | head -3 \
        || echo "  (nesasm check failed — round-trip tooling unavailable, game build unaffected)"
else
    echo "WARNING: pip install failed — continuing without Python deps (nesasm/round-trip tooling unavailable, game build unaffected)."
fi

# ---- NESRecomp submodule ----
if [ -d "${SCRIPT_DIR}/external/nesrecomp" ]; then
    echo ""
    echo "Updating NESRecomp submodule..."
    cd "${SCRIPT_DIR}/external/nesrecomp"
    git fetch origin 2>&1
    git checkout origin/master 2>&1
    cd "${SCRIPT_DIR}"
else
    echo ""
    echo "Initializing NESRecomp submodule..."
    git submodule update --init --recursive -- "${SCRIPT_DIR}/external/nesrecomp" 2>&1
    cd "${SCRIPT_DIR}/external/nesrecomp"
    git fetch origin 2>&1
    git checkout origin/master 2>&1
    cd "${SCRIPT_DIR}"
fi

# ---- Game-specific runner patches (re-applied after every submodule checkout) ----
# Active patches live in patches/*.patch (sorted order); patches/archived/
# holds dormant ones that setup.sh deliberately ignores. See
# patches/active-patches.md.
if [ -d "${SCRIPT_DIR}/patches" ]; then
    echo ""
    echo "Applying game patches to NESRecomp submodule..."
    cd "${SCRIPT_DIR}/external/nesrecomp"
    for p in "${SCRIPT_DIR}"/patches/*.patch; do
        [ -e "$p" ] || break
        if git apply --check "$p" 2>/dev/null; then
            git apply "$p" 2>&1 && echo "  applied: $(basename "$p")"
        else
            echo "  skipped (already applied or conflicts): $(basename "$p")"
        fi
    done
    cd "${SCRIPT_DIR}"
fi

# ---- MesenCE (build or use pre-built binary) ----
mkdir -p "${MESENCE_DIR}"

if [ -f "${MESEN_BIN}" ]; then
    echo ""
    echo "Mesen binary already exists at ${MESEN_BIN} — skipping build."
else
    if [ -d "${MESENCE_DIR}/.git" ]; then
        echo ""
        echo "Updating MesenCE source..."
        cd "${MESENCE_DIR}"
        git fetch origin 2>&1
        git checkout master 2>&1
        cd "${SCRIPT_DIR}"
    else
        echo ""
        echo "Cloning MesenCE source..."
        git clone https://github.com/nesdev-org/MesenCE "${SCRIPT_DIR}/external/mesence" 2>&1
    fi

    echo ""
    echo "Building MesenCE..."
    cd "${MESENCE_DIR}"
    USE_GCC=true make clean 2>&1 || true
    USE_GCC=true make -j$(nproc) 2>&1 || echo "WARNING: MesenCE build failed. Download the pre-built binary instead."
    cd "${SCRIPT_DIR}"

    if [ ! -f "${MESEN_BIN}" ]; then
        echo "No Mesen binary at ${MESEN_BIN}."
        echo "Download from: https://github.com/nesdev-org/MesenCE/releases/download/2.2.1/Mesen_2.2.1_Linux_x64.zip"
    fi
fi
echo "Mesen binary ready: ${MESEN_BIN}"

# ---- blargg test ROMs (tools/blargg harness) ----
TEST_ROMS_DIR="${SCRIPT_DIR}/external/nes-test-roms"
if [ -d "${TEST_ROMS_DIR}" ]; then
    echo ""
    echo "Updating nes-test-roms..."
    git -C "${TEST_ROMS_DIR}" pull --ff-only 2>&1 | tail -1 || echo "  (update failed — continuing with existing checkout)"
else
    echo ""
    echo "Cloning nes-test-roms (blargg suite)..."
    git clone --depth 1 https://github.com/christopherpow/nes-test-roms "${TEST_ROMS_DIR}" 2>&1 | tail -1 || echo "  (clone failed — run: python3 tools/blargg/run_blargg.py --fetch)"
fi

# ---- Frame generation tools ----
echo ""
echo "Setting up frame generation tools..."
mkdir -p "${FRAME_GEN_DIR}"

# ---- Build the runner ----
echo ""
echo "Building NESRecomp runner..."
cmake -S "${SCRIPT_DIR}/src" -B "${SCRIPT_DIR}/build" -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1
cmake --build "${SCRIPT_DIR}/build" -j$(nproc) 2>&1

# ---- Verify setup ----
echo ""
echo "=== Setup complete ==="
echo ""
echo "To build the game: cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)"
echo "To run: ./build/super_dodgeball \"roms/Super Dodge Ball (USA).nes\" [--smoke N] [--save-screenshot DIR]"
echo ""
echo "=== MesenCE / Frame Generation ==="
echo "  Mesen binary: ${MESEN_BIN}"
echo "  Frame export: ${FRAME_GEN_DIR}/export_frames.lua"
echo ""
echo "Generate reference frames with Mesen:"
echo "  MESEN_BIN=${MESEN_BIN} \\"
echo "    ./tools/frame_gen/run_frames.sh \"roms/Super Dodge Ball (USA).nes\" 100 nes_reference --start"
echo ""
echo "Then compare frames:"
echo "  ./build/super_dodgeball \"rom.nes\" --smoke 100 --save-screenshot recomp_output"
echo "  python3 tools/compare_frames.py --ref nes_reference --recomp recomp_output --frames 100"
echo ""

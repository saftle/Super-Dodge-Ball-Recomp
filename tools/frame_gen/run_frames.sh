#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../" && pwd)"

ROM="${1:?Usage: $0 <rom> <num_frames> <output_dir> [--start]}"
NUM_FRAMES="${2:?Usage: $0 <rom> <num_frames> <output_dir> [--start]}"
OUTPUT_DIR="${3:-nes_reference}"
START_FLAG="${4:-}"

MESEN_BIN="${MESEN_BIN:-${PROJECT_DIR}/external/mesence/Mesen}"
MESENCE_DIR="${PROJECT_DIR}/external/mesence"
CONFIG_DIR=$(mktemp -d)

if [ ! -x "$MESEN_BIN" ]; then
    echo "ERROR: Mesen binary not found at ${MESEN_BIN}"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"

# Copy the Mesen binary AND its shared libraries into the sandbox. MesenCE
# resolves settings.json from the directory the binary sits in, NOT from $HOME,
# so the sandbox has to own the executable -- overriding HOME alone silently
# leaves us running the real user config (verified: setting Nes.Region=Pal only
# took effect once settings.json sat beside a copied binary). See
# docs/research.md section 23.
cp "${MESENCE_DIR}/Mesen" "$CONFIG_DIR/Mesen"
cp "${MESENCE_DIR}/MesenCore.so" "$CONFIG_DIR/" 2>/dev/null || true
cp "${MESENCE_DIR}/libHarfBuzzSharp.so" "$CONFIG_DIR/" 2>/dev/null || true
cp "${MESENCE_DIR}/libSkiaSharp.so" "$CONFIG_DIR/" 2>/dev/null || true
cp "${MESENCE_DIR}/settings.json" "$CONFIG_DIR/settings.json"
cp "${SCRIPT_DIR}/export_frames.lua" "$CONFIG_DIR/export_frames.lua"

# Patch Lua script
sed -i "s/local max_frames = 10/local max_frames = ${NUM_FRAMES}/" "$CONFIG_DIR/export_frames.lua"
sed -i "s|local output_dir = \"/tmp/nes_reference\"|local output_dir = \"${OUTPUT_DIR}\"|" "$CONFIG_DIR/export_frames.lua"

export HOME="$CONFIG_DIR"
# Prebuilt .NET binaries can need this to run on a newer runtime than they
# target; harmless otherwise.
export DOTNET_ROLL_FORWARD="${DOTNET_ROLL_FORWARD:-Major}"

echo "Rendering ${NUM_FRAMES} frames from ${ROM}..."
echo "Output: ${OUTPUT_DIR}/"
echo "Mesen: ${MESEN_BIN}"

# Absolute paths: we cd into the sandbox to invoke ./Mesen, so a relative ROM
# path would no longer resolve from the new working directory.
ROM="$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")"
[ -f "$ROM" ] || { echo "ERROR: ROM not found: $ROM" >&2; exit 1; }

# Run from inside the sandbox with a RELATIVE program name. xvfb-run execs via
# `sh -c`, which word-splits a program path containing spaces (this repo's own
# path has one) even when it is quoted. The ROM and Lua paths are argv operands
# and survive intact.
( cd "$CONFIG_DIR" && xvfb-run -a ./Mesen --testRunner "${ROM}" ./export_frames.lua --timeout=120 ) 2>&1 || true

if compgen -G "${OUTPUT_DIR}/frame_*.png" > /dev/null; then
  echo "Done! Frames saved to ${OUTPUT_DIR}/"
else
  # A silently-empty output dir reads as "rendered nothing" instead of
  # "Mesen never ran", which is the failure that hides a broken config.
  echo "ERROR: no frames written to ${OUTPUT_DIR}/ -- Mesen did not run." >&2
  echo "       Check for an open Mesen window (testRunner not engaged) and that" >&2
  echo "       external/mesence/settings.json + the Mesen binary are present." >&2
fi
rm -rf "$CONFIG_DIR"

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

# Copy config and libraries
cp "${MESENCE_DIR}/settings.json" "$CONFIG_DIR/settings.json"
cp "${SCRIPT_DIR}/export_frames.lua" "$CONFIG_DIR/export_frames.lua"
cp "${MESENCE_DIR}/MesenCore.so" "$CONFIG_DIR/" 2>/dev/null || true
cp "${MESENCE_DIR}/libHarfBuzzSharp.so" "$CONFIG_DIR/" 2>/dev/null || true
cp "${MESENCE_DIR}/libSkiaSharp.so" "$CONFIG_DIR/" 2>/dev/null || true

# Patch Lua script
sed -i "s/local max_frames = 10/local max_frames = ${NUM_FRAMES}/" "$CONFIG_DIR/export_frames.lua"
sed -i "s|local output_dir = \"/tmp/nes_reference\"|local output_dir = \"${OUTPUT_DIR}\"|" "$CONFIG_DIR/export_frames.lua"

export HOME="$CONFIG_DIR"

echo "Rendering ${NUM_FRAMES} frames from ${ROM}..."
echo "Output: ${OUTPUT_DIR}/"
echo "Mesen: ${MESEN_BIN}"

xvfb-run -a "$MESEN_BIN" --testRunner "${ROM}" "$CONFIG_DIR/export_frames.lua" --timeout=120 2>&1 || true

echo "Done! Frames saved to ${OUTPUT_DIR}/"
rm -rf "$CONFIG_DIR"

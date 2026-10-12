#!/usr/bin/env bash
# PPU overclocking oracle harness — drives Super Dodge Ball into a real match
# under MesenCE at a chosen Nes.PpuExtraScanlinesBeforeNmi, and logs per-frame
# state so stock vs overclocked can be compared.
#
# Usage: run_overclock.sh <before_nmi> <max_frames> <tag>
#   run_overclock.sh 0    1200 stock
#   run_overclock.sh 300  1200 oc300
#
# Backs docs/research.md section 23. Reproduces the "+12 extra menu-loop laps
# per frame" measurement that shows PPU overclocking speeds this game up.
#
# Three harness traps, each of which silently produces "no difference"
# (see AGENTS.md "MesenCE headless settings"):
#  1. MesenCE reads settings.json from the directory its binary sits in, NOT
#     $HOME. Isolating HOME does nothing -- you get the real user config.
#  2. xvfb-run execs via `sh -c` and word-splits a program path containing
#     spaces even when quoted, so invoke ./Mesen from inside the work dir.
#  3. After that cd, a relative ROM path no longer resolves -- use absolute.
#
# Control: if `cycles/frame` printed at the end is still 29781 for a non-zero
# before_nmi, the setting did NOT apply and the run is worthless. It should be
# ~29781 + before_nmi*341/3.
set -uo pipefail
BEFORE="${1:-0}"; FRAMES="${2:-1200}"; TAG="${3:-stock}"
P="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
W="$P/tmp/overclock_run_$TAG"; rm -rf "$W"; mkdir -p "$W"

cp "$P/external/mesence/Mesen" \
   "$P/external/mesence/MesenCore.so" \
   "$P/external/mesence/libHarfBuzzSharp.so" \
   "$P/external/mesence/libSkiaSharp.so" \
   "$W/"
cp "$P/external/mesence/settings.json" "$W/settings.json"

python3 - "$W/settings.json" "$BEFORE" <<'PY'
import json, io, sys
p, b = sys.argv[1], int(sys.argv[2])
d = json.load(io.open(p, encoding='utf-8-sig'))
n = d['Nes']
n['PpuExtraScanlinesBeforeNmi'] = b
n['PpuExtraScanlinesAfterNmi'] = 0
n['Region'] = 'Ntsc'
n['RamPowerOnState'] = 'AllZeros'
n['RandomizeMapperPowerOnState'] = False
n['RandomizeCpuPpuAlignment'] = False
d['Debug']['BreakOnCpuCrash'] = False
d['Debug']['EnableTestMode'] = True
d['Debug'].setdefault('ScriptWindow', {})['AllowIoOsAccess'] = True
d['Debug']['ScriptWindow']['ScriptTimeout'] = 600
json.dump(d, io.open(p, 'w', encoding='utf-8'), indent=2)
# Read back through a parser: "Snes" carries a same-named key earlier in the
# file, so a naive grep can report the wrong console's value.
print("  Nes.PpuExtraScanlinesBeforeNmi =",
      json.load(io.open(p, encoding='utf-8'))['Nes']['PpuExtraScanlinesBeforeNmi'])
PY

cp "$P/tools/overclock/overclock_probe.lua" "$W/m.lua"
rm -f "$P/external/mesence/mesen.lock"

export DOTNET_ROLL_FORWARD="${DOTNET_ROLL_FORWARD:-Major}"
export MAX_FRAMES="$FRAMES"
export SHOT_DIR="$W"
export SHOT_EVERY=1000000
export OUT_FILE="$W/m.csv"

echo "=== $TAG: before=$BEFORE frames=$FRAMES ==="
( cd "$W" && xvfb-run -a ./Mesen --testRunner \
    "$P/roms/Super Dodge Ball (USA).nes" ./m.lua --timeout=600 ) 2>&1 \
  | grep -viE "uninitialized memory read" | tail -5

if [ -s "$W/m.csv" ]; then
  CYCLES=$(awk -F, 'NR>10{s+=$2;n++}END{if(n)printf "%.0f",s/n}' "$W/m.csv")
  EXPECT=$(awk -v b="$BEFORE" 'BEGIN{printf "%.0f", 29781 + b*341/3}')
  echo "  rows: $(wc -l < "$W/m.csv")"
  echo "  cycles/frame: $CYCLES (expected ~$EXPECT for before=$BEFORE)"
  if [ "$BEFORE" != "0" ] && [ "$CYCLES" -lt 30000 ]; then
    echo "  WARNING: cycles/frame unchanged -- the setting did NOT apply." >&2
    echo "           This run cannot be used to compare. See header notes." >&2
  fi
  echo "  csv: $W/m.csv"
else
  echo "  NO OUTPUT (run failed)"
fi
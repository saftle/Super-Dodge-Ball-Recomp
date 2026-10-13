#!/bin/bash
# play_record.sh - play the game in a window and capture the run as a vector.
#
# Hand-written repro vectors are guesswork, and this project's record shows how
# that goes: the game silently drops input aimed before frame 594, and a margin
# artifact needs the camera to pan the whole way before it shows anything.
# Neither is expressible as a schedule someone typed. This plays the game, and
# records what the guest actually read, so the same run can be replayed
# headless and analysed frame by frame.
#
#   tools/play_record.sh <name> [frames]
#
# Everything lands in tmp/<name>/, one folder per run (AGENTS.md), and the
# folder is left holding:
#
#   repro.input   the recorded vector, replayable with `nes_game --input`
#   trace.log     SDB_TRACE=1, the per-frame sprite/margin/widescreen lines
#   w_*.png       a screenshot every SDB_SHOT_EVERY frames, AS THE WINDOW SHOWS IT
#   mods/         a private copy of the mod catalog, so no run can share state.toml
#
# Then it prints the headless replay command, which is the useful half: the
# window is for finding the artifact, the replay is for measuring it.
#
# Environment knobs, all optional:
#
#   SDB_ASPECT      16:9 | 21:9 | 32:9 | stock        (default 16:9)
#   SDB_OVERSCAN    1 reveals the 8 hidden left columns (default 1)
#   SDB_FLICKER     1 runs the character-flicker mod    (default 1)
#   SDB_SHOT_EVERY  screenshot every N frames           (default 1, which is only
#                   affordable because the scale is pinned -- see the measured
#                   note on the config below before raising WindowScale)
#   SDB_SCALE       window scale for the capture        (default 1; raise to 2 if
#                   426x240 is awkward to play, PNGs go 300 KB -> 1.2 MB)
#   SDB_FRAMES      default frame count, overridable by the second argument   (2000)
#   SDB_LAUNCHER    1 keeps the recomp-ui launcher; 0 skips it  (default 0)
#   SDB_UNCAPPED    1 runs as fast as the machine can    (default 0, frame-paced)
#
# WHY --ram-init zeros is passed HERE and not only in the replay: the flag does
# not imply headless (cyc_host.c:1044), so the windowed run can take it too --
# and it must, or the recording is one sample of a random boot and the replay is
# a different run. The game is random; without zeros the vector will not
# reproduce, and that reads as "the artifact did not reproduce" rather than as
# the mistake it is.
set -e
cd "$(dirname "$0")/.."

NAME="${1:-}"
if [ -z "$NAME" ]; then
    echo "usage: tools/play_record.sh <name> [frames]" >&2
    echo "       see the header comment for the SDB_* knobs" >&2
    exit 2
fi
FRAMES="${2:-${SDB_FRAMES:-2000}}"
ASPECT="${SDB_ASPECT:-16:9}"
OVERSCAN="${SDB_OVERSCAN:-1}"
FLICKER="${SDB_FLICKER:-1}"
SHOT_EVERY="${SDB_SHOT_EVERY:-1}"
SCALE="${SDB_SCALE:-1}"
LAUNCHER="${SDB_LAUNCHER:-0}"
UNCAPPED="${SDB_UNCAPPED:-0}"

ROM="roms/Super Dodge Ball (USA).nes"
GAME="./build-cycle/nes_game"
OUT="tmp/$NAME"

if [ ! -f "$ROM" ]; then
    echo "play_record: no ROM at $ROM" >&2
    exit 1
fi
if [ ! -x "$GAME" ]; then
    echo "play_record: $GAME not built -- run ./build_and_run.sh once first" >&2
    exit 1
fi

mkdir -p "$OUT"

# Clear only what this script writes, so re-running the same name cannot leave
# last run's screenshots interleaved with this one's -- a half-old, half-new
# frame sequence is worse than either, because it looks like a real artifact.
rm -f "$OUT"/w_*.png "$OUT"/trace.log "$OUT"/repro.input

# PIN THE RESOLUTION, or the screenshots make the game run in slow motion.
#
# Measured 2026-10-13, 600 frames, everything else identical:
#
#   fullscreen + WindowScale 3, --present-every 4   41.5 fps
#   windowed    + WindowScale 1, --present-every 1   53.9 fps
#
# The windowed --present-out path is SDL_RenderReadPixels on the RENDERER plus a
# PNG encode. At fullscreen scale 3 that is a 1080p frame -- 14.87 MB every four
# frames -- and encoding 2M pixels costs ~150 ms. That blows the 16.6 ms frame
# budget, so the pacing loop keeps taking its "fell behind, don't try to catch
# up" fallback (cyc_sdl.c:1540) and the whole run drops to about 2/3 speed, with
# the audio, because audio is generated per emulated frame. It gets worse as the
# run goes on because the dirty-page writeback queue fills.
#
# At 1x the same shot is 300 KB and every frame is affordable. So this derives a
# config that pins the display keys and changes nothing else -- in particular
# Player1Source, because player 1 is the KEYBOARD and player 2 is the controller
# (cyc_input.c): a config that loses that records the whole session on port 2.
sed -e "s/^WindowScale = .*/WindowScale = $SCALE/" \
    -e 's/^Fullscreen = .*/Fullscreen = 0/' \
    -e 's/^VSync = .*/VSync = 0/' \
    build-cycle/config.ini > "$OUT/config.ini"

# A private copy of the catalog. build-cycle/mods/state.toml is SHARED MUTABLE
# state, and two concurrent runs can leave a feature enabled=false, which makes
# the whole run silently go stock. One nes_game per mods dir.
rm -rf "$OUT/mods"
cp -r build-cycle/mods "$OUT/mods"

# [feature.values] binds to the [[feature]] declared IMMEDIATELY above it, so a
# values table in the wrong place is silently dropped and the feature falls back
# to its manifest default with no warning anywhere (research.md 25). The
# widescreen values table therefore comes last, directly after widescreen.
ov=$( [ "$OVERSCAN" = "1" ] && echo true || echo false )
fl=$( [ "$FLICKER" = "1" ] && echo true || echo false )
cat > "$OUT/mods/state.toml" <<EOF
format_version = 1

[[package]]
id = "super-dodgeball.character-flicker"
version = "1.0.0"

[[package]]
id = "super-dodgeball.ppu-overclock"
version = "1.0.0"

[[package]]
id = "super-dodgeball.widescreen"
version = "1.0.0"

[[feature]]
package_id = "super-dodgeball.character-flicker"
id = "remove-character-flicker"
enabled = $fl

[[feature]]
package_id = "super-dodgeball.ppu-overclock"
id = "ppu-overclock"
enabled = false

[[feature]]
package_id = "super-dodgeball.widescreen"
id = "overscan"
enabled = $ov

[[feature]]
package_id = "super-dodgeball.widescreen"
id = "widescreen"
enabled = true

[feature.values]
aspect = "$ASPECT"
EOF

echo "=== $NAME: $FRAMES frames, $ASPECT, overscan=$OVERSCAN, flicker=$FLICKER ==="
echo "    vector  -> $OUT/repro.input"
echo "    trace   -> $OUT/trace.log"
echo "    shots   -> $OUT/w_<frame>.png (every $SHOT_EVERY)"
echo
echo "Play. Close the window (or let --exit-after stop it) when you have it."
echo

# The windowed --present-out captures the RENDERER, so these PNGs are at window
# size, not at the presented width. They are for seeing it happen; the replay
# below writes the canonical 426x240 (or whatever the aspect gives) images.
#
# stdout AND stderr both go to the log, not just stderr. That looks redundant
# until you run this under xvfb, which ends its exec with `"$@" 2>&1`
# (/usr/bin/xvfb-run:184) and so overrides a plain `2> file` -- the trace goes
# to the terminal and the log comes out empty, which is a trace that silently
# measured nothing.
set +e
SDB_TRACE=1 NESRECOMP_NO_LAUNCHER=$( [ "$LAUNCHER" = "1" ] && echo 0 || echo 1 ) \
"$GAME" "$ROM" \
  --config "$OUT/config.ini" \
  --ram-init zeros \
  --mods-root "$OUT/mods" \
  --widescreen "$ASPECT" \
  --overscan \
  --record-input "$OUT/repro.input" \
  --present-out "$OUT/w_" --present-every "$SHOT_EVERY" \
  --exit-after "$FRAMES" \
  $( [ "$UNCAPPED" = "1" ] && echo --uncapped ) \
  > "$OUT/trace.log" 2>&1
RC=$?
set -e

if [ ! -s "$OUT/repro.input" ]; then
    echo
    echo "play_record: no vector written -- see $OUT/trace.log" >&2
    exit 1
fi

# The frame count is on the vector's last line; --frames must match it or the
# replay covers a different number of frames than the recording did. The line is
# a COMMENT ("# 200 frames, 41 input steps"), so do not anchor on ^ -- that
# matches nothing, `set -e` then kills the script silently right here, and the
# run looks like it worked with no summary.
REC_FRAMES=$(grep -oE '[0-9]+ frames' "$OUT/repro.input" | tail -1 | grep -oE '^[0-9]+' || true)
REC_STEPS=$(grep -oE '[0-9]+ input steps' "$OUT/repro.input" | tail -1 | grep -oE '^[0-9]+' || true)
: "${REC_FRAMES:=$FRAMES}"
: "${REC_STEPS:=?}"

# Append the exact command, so the vector documents itself next to whatever else
# was on for this capture.
cat >> "$OUT/repro.input" <<EOF
#
# --- replay (what this capture was) ---
#   cd "$PWD" && SDB_TRACE=1 ./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" \\
#       --frames $REC_FRAMES --input $OUT/repro.input --ram-init zeros \\
#       --mods-root $OUT/mods --widescreen $ASPECT --overscan \\
#       --present-out $OUT/r_ --present-every 1
#
# MEASURED 2026-10-13, and do not assume it: this replay is NOT byte-faithful to
# the windowed capture it came from. Two windowed runs of the same schedule are
# byte-identical to each other (same vector, same trace), but a headless replay of
# that vector diverges from both at frame 4 -- scroll 32 vs 256, 496 matches on
# 0-3 -- and ends up on a different game state (2666 vs 2121 off-edge captures,
# 32200 vs 15504 margin sprites drawn, 769 vs 752 frames composed). The input is
# not the cause: the recorded frames are the exact frames the virtual schedule
# fired on, and the divergence starts four frames before the first press.
# So the window and the headless host are not running the same machine; that is
# an open engine question, not something this file can fix.
#
# Until that is settled, ANALYSE THE CAPTURE IN PLACE: trace.log is the ground
# truth for what was on screen, w_*.png is what it looked like, and
# tools/widescreen/margin_track.py audits trace.log directly.
EOF

echo
echo "=== recorded $REC_FRAMES frames, $REC_STEPS input steps (exit $RC) ==="
grep -E "^(widescreen|margin sprites):" "$OUT/trace.log" || true
echo
echo "Replay it with:"
echo "  SDB_TRACE=1 ./build-cycle/nes_game \"$ROM\" \\"
echo "      --frames $REC_FRAMES --input $OUT/repro.input --ram-init zeros \\"
echo "      --mods-root $OUT/mods --widescreen $ASPECT --overscan \\"
echo "      --present-out $OUT/r_ --present-every 1 2> $OUT/replay.log"
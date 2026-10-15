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
#   SDB_MODS        which mods are on: a list of feature ids, any of the three,
#                   in any combination. Comma- or space-separated, or repeated
#                   as SDB_MODS_FLAG.
#
#                     remove-character-flicker  the sprite fix on its own
#                     widescreen                the wide canvas, SDB_ASPECT
#                     ppu-overclock             extra CPU time; CHANGES TIMING,
#                                               so a capture with it on does
#                                               not compare against any stock
#                                               figure in the docs
#
#                   default  remove-character-flicker,widescreen   (the shipped
#                            configuration: both presentation mods, no timing
#                            change)
#                   SDB_MODS_FLAG=1          repeatable, ORs the names together
#
#                     none     SDB_MODS=                      -> stock
#                     one      SDB_MODS=remove-character-flicker
#                     two      SDB_MODS=remove-character-flicker,widescreen
#                     three    SDB_MODS=remove-character-flicker,widescreen,ppu-overclock
#
#                   A name this script does not know is a typo, refused here with
#                   the valid list -- the catalog's own refusal ("does not
#                   provide this trusted plugin") names a PACKAGE, which leaves
#                   you guessing which of the three features you got wrong.
#   SDB_ASPECT      aspect for widescreen: 16:9 | 21:9 | 32:9 | stock  (16:9)
#   SDB_SCANLINES   ppu-overclock scanlines, 0..262   (default 262, the manifest's)
#   SDB_SHOT_EVERY  PRESENT every N frames               (default 1, which is only
#                   affordable because the scale is pinned -- see the measured
#                   note on the config below before raising WindowScale).
#                   This is a PRESENT interval, not a screenshot interval: the
#                   compositor only runs on a presented frame, so N > 1 silently
#                   skips it and every `drawn`/`composed` count drops. Measure at 1.
#   SDB_SCALE       window scale for the capture        (default 1; raise to 2 if
#                   426x240 is awkward to play, PNGs go 300 KB -> 1.2 MB)
#   SDB_FRAMES      default frame count, overridable by the second argument   (2000)
#   SDB_UNLIMITED   1 runs until you CLOSE THE WINDOW, ignoring SDB_FRAMES and
#                   omitting --exit-after            (default 0)
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
UNLIMITED="${SDB_UNLIMITED:-0}"
ASPECT="${SDB_ASPECT:-16:9}"
SCANLINES="${SDB_SCANLINES:-262}"

# Every feature the catalog ships, in catalog order. Written out rather than
# scraped from the manifests so a typo in SDB_MODS fails here with a usable
# message. Packages map one-to-one onto features here; if a package ever grows a
# second feature this list is the place that has to learn about it.
FEATURES=(remove-character-flicker ppu-overclock widescreen)
feat_pkg() {
    case "$1" in
        remove-character-flicker) echo super-dodgeball.character-flicker ;;
        ppu-overclock)           echo super-dodgeball.ppu-overclock ;;
        widescreen)              echo super-dodgeball.widescreen ;;
        *) return 1 ;;
    esac
}

# SDB_MODS is the whole of "which mods are on": a list of feature ids, so one, two
# or three all work and nothing is positional. Comma- or space-separated. The
# default is the shipped configuration: both presentation mods, no timing change.
# SDB_MODS_FLAG is the repeatable form, ORed on top, for callers that build the
# list up one name at a time.
# NOTE the `-` and not `:-`. `${VAR:-default}` fires when VAR is empty as well as
# unset, so `SDB_MODS=` would silently fall back to the default and you could
# never turn every mod off -- which is exactly what the "mod off = stock" gate
# needs, so the gate would have been measuring a mod-on run and calling it stock.
WANT="${SDB_MODS-remove-character-flicker,widescreen}"
if [ -n "${SDB_MODS_FLAG:-}" ]; then WANT="$WANT,$SDB_MODS_FLAG"; fi
declare -A ON=()
for tok in ${WANT//,/ }; do
    [ -n "$tok" ] || continue
    if ! feat_pkg "$tok" >/dev/null; then
        echo "play_record: SDB_MODS names no feature '$tok'." >&2
        echo "             valid: ${FEATURES[*]}" >&2
        exit 2
    fi
    ON[$tok]=true
done

case "$SCANLINES" in
    ''|*[!0-9]*)
        echo "play_record: SDB_SCANLINES must be 0..262, got '$SCANLINES'" >&2
        exit 2 ;;
esac
if [ "$SCANLINES" -gt 262 ]; then
    echo "play_record: SDB_SCANLINES must be 0..262, got $SCANLINES" >&2
    exit 2
fi
SHOT_EVERY="${SDB_SHOT_EVERY:-1}"
SCALE="${SDB_SCALE:-1}"
LAUNCHER="${SDB_LAUNCHER:-0}"
UNCAPPED="${SDB_UNCAPPED:-0}"

# --uncapped is rejected by the engine unless --exit-after is positive
# (cyc_sdl.c:1238), so the two are mutually exclusive. Fail here rather than let
# the run die with a message the player is unlikely to connect to their inputs.
if [ "$UNCAPPED" = "1" ] && [ "$UNLIMITED" = "1" ]; then
    echo "play_record: SDB_UNCAPPED=1 needs SDB_UNLIMITED=0 (--uncapped requires" >&2
    echo "             a positive --exit-after limit; cyc_sdl.c:1238)" >&2
    exit 2
fi

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
# to its manifest default with no warning anywhere (research.md 25). Two
# consequences here:
#   - each values table is emitted directly beneath its own feature, and
#   - a DISABLED feature gets no values table at all, because a stray one would
#     bind to the NEXT feature instead and be dropped against the wrong one.
# That is the bug that made `widescreen` fall back to `fit` once already.
{
    echo "format_version = 1"
    echo
    for f in "${FEATURES[@]}"; do
        echo "[[package]]"
        echo "id = \"$(feat_pkg "$f")\""
        echo "version = \"1.0.0\""
        echo
    done
    for f in "${FEATURES[@]}"; do
        echo "[[feature]]"
        echo "package_id = \"$(feat_pkg "$f")\""
        echo "id = \"$f\""
        echo "enabled = ${ON[$f]:-false}"
        case "$f" in
            widescreen)
                if [ -n "${ON[$f]:-}" ]; then
                    echo
                    echo "[feature.values]"
                    echo "aspect = \"$ASPECT\""
                fi ;;
            ppu-overclock)
                if [ -n "${ON[$f]:-}" ]; then
                    echo
                    echo "[feature.values]"
                    echo "scanlines = $SCANLINES"
                fi ;;
        esac
        echo
    done
} > "$OUT/mods/state.toml"

# --widescreen is NOT the same thing as the widescreen feature. The flag is
# handled in our own option parser and calls sdb_ws_set_aspect() directly, so it
# installs the compositor whether or not the feature is in state.toml. Passing it
# for a flicker-only capture would quietly turn widescreen on anyway -- which is
# exactly the class of bug this knob exists to stop, so it follows SDB_MODS.
WS_ON=${ON[widescreen]:-false}
# In catalog order, not hash order: this line is printed at the top of the run and
# appended to the vector, and a list that reshuffles between runs makes two
# captures hard to tell apart at a glance.
MODS_ON=""
for f in "${FEATURES[@]}"; do
    if [ -n "${ON[$f]:-}" ]; then MODS_ON="$MODS_ON${MODS_ON:+, }$f"; fi
done
[ -n "$MODS_ON" ] || MODS_ON="(none -- stock)"

if [ "$UNLIMITED" = "1" ]; then
    echo "=== $NAME: UNLIMITED (no --exit-after), mods: $MODS_ON ==="
else
    echo "=== $NAME: $FRAMES frames, mods: $MODS_ON ==="
fi
# An `&&` on its own line would abort the script under `set -e` when false, so
# this is an `if`.
if [ "$WS_ON" = "true" ]; then echo "    aspect: $ASPECT"; fi
echo "    vector  -> $OUT/repro.input"
echo "    trace   -> $OUT/trace.log"
echo "    shots   -> $OUT/w_<frame>.png (every $SHOT_EVERY)"
echo
echo "Play. Close the window (or let --exit-after stop it) when you have it."
echo
if [ "$UNLIMITED" = "1" ]; then
    # Dropping --exit-after is safe for the vector: sdb_rec_write fflushes every
    # line, and the "# N frames, M input steps" trailer a replay needs is written
    # by sdb_rec_close(), which runs from the DESTRUCTOR -- the one place that
    # fires on a window close as well as on --exit-after
    # (src/sdb_host_extras.c:1363, :1378-1382). So close the window normally and
    # the vector is complete. What must NOT happen is SIGKILL, which skips the
    # destructor and leaves a vector with no frame count, which then replays too
    # little or too much and reads as "the artifact did not reproduce".
    echo "No frame limit: close the window when you have it. Close it with the"
    echo "window button or a quit key -- do NOT SIGKILL it, or the vector loses"
    echo "its frame count and the replay covers the wrong number of frames."
    echo
fi

# The windowed --present-out captures the RENDERER, so these PNGs are at window
# size, not at the presented width. They are for seeing it happen; the replay
# below writes the canonical 426x240 (or whatever the aspect gives) images.
#
# stdout AND stderr both go to the log, not just stderr. That looks redundant
# until you run this under xvfb, which ends its exec with `"$@" 2>&1`
# (/usr/bin/xvfb-run:184) and so overrides a plain `2> file` -- the trace goes
# to the terminal and the log comes out empty, which is a trace that silently
# measured nothing.
ARGS=( "$ROM"
  --config "$OUT/config.ini"
  --ram-init zeros
  --mods-root "$OUT/mods"
  --record-input "$OUT/repro.input"
  --present-out "$OUT/w_" --present-every "$SHOT_EVERY" )
if [ "$WS_ON" = "true" ]; then ARGS+=( --widescreen "$ASPECT" ); fi
if [ "$UNLIMITED" != "1" ]; then ARGS+=( --exit-after "$FRAMES" ); fi
if [ "$UNCAPPED" = "1" ]; then ARGS+=( --uncapped ); fi

set +e
SDB_TRACE=1 NESRECOMP_NO_LAUNCHER=$( [ "$LAUNCHER" = "1" ] && echo 0 || echo 1 ) \
"$GAME" "${ARGS[@]}" > "$OUT/trace.log" 2>&1
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
#
# Anchor on `^# ` plus a DIGIT, and take the FIRST match, not the last. The
# script appends a long comment block below this that contains the phrase in
# prose ("769 vs 752 frames composed"), so an unanchored `tail -1` reads THAT
# number and replays the wrong length. Measured: a 287-frame capture replayed
# 752 frames. `^# [0-9]+ frames` matches only the destructor's trailer.
REC_FRAMES=$(grep -oE '^# [0-9]+ frames' "$OUT/repro.input" | head -1 | grep -oE '[0-9]+' || true)
REC_STEPS=$(grep -oE '^# [0-9]+ frames, [0-9]+ input steps' "$OUT/repro.input" | head -1 | grep -oE '[0-9]+ input' | grep -oE '^[0-9]+' || true)
: "${REC_FRAMES:=$FRAMES}"
: "${REC_STEPS:=?}"

# Append the exact command, so the vector documents itself next to whatever else
# was on for this capture. WS_FLAG is empty when widescreen was off, and the
# replay then must NOT pass --widescreen -- the flag installs the compositor by
# itself, so a replay that "helpfully" added it would be measuring a different
# picture than the capture it came from.
WS_FLAG=""
if [ "$WS_ON" = "true" ]; then WS_FLAG="--widescreen $ASPECT"; fi

# The overclock has to reach the replay, not just widescreen. The printed
# command used to carry only --widescreen, so a replay of an overclocked capture
# silently dropped --ppu-overclock -- and the overclock changes the guest's
# timing, so the replay would not even reach the same game state. Built from the
# same SDB_MODS the run used, so it cannot drift apart from it.
EXTRA_FLAG=""
if [ "${ON[ppu-overclock]:-false}" = "true" ]; then
    EXTRA_FLAG="$EXTRA_FLAG --ppu-overclock $SCANLINES"
fi

cat >> "$OUT/repro.input" <<EOF
#
# --- replay (what this capture was) ---
#   mods on: $MODS_ON
#
#   cd "$PWD" && SDB_TRACE=1 ./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" \\
#       --frames $REC_FRAMES --input $OUT/repro.input --ram-init zeros \\
#       --mods-root $OUT/mods $WS_FLAG$EXTRA_FLAG \\
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
echo "      --mods-root $OUT/mods $WS_FLAG$EXTRA_FLAG \\"
echo "      --present-out $OUT/r_ --present-every 1 2> $OUT/replay.log"

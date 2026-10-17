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
#   state.bin     a save state at SDB_ANCHOR, so the run can be REPLAYED rather
#                 than merely re-executed
#   probe.bin     a second save state, SDB_ANCHOR_PROBE frames later: the machine
#                 the replay has to come back to, as a file to compare
#   anchor.json   the frame numbers, so nothing downstream recomputes them
#   anchor.log    what the anchor helper had to say
#
# Then it prints the headless replay command, which is the useful half: the
# window is for finding the artifact, the replay is for measuring it.
#
# WHY THERE IS A SAVE STATE AND NOT ONLY A VECTOR
# ------------------------------------------------
# This game is random, and a vector does not carry a run -- it carries the
# INPUTS to one. A headless replay of a recorded windowed vector is a different
# machine from frame 4 (AGENTS.md, "Windowed and headless run different guest
# state"), and AGENTS.md's own verdict on the old plan is the reason this is
# worth building: a replay comparison "reported 0 differing pixels over 1263
# frames" on a build that visibly doubled a running player. A green replay is
# not weak evidence here, it is evidence of nothing.
#
# A save state pins the whole machine at one frame -- CPU, RAM, PPU, mapper,
# APU, the host's frame counter, the mods' records -- so the game's RNG, which
# is ordinary guest RAM, is pinned with the rest. Loading it makes the headless
# replay BE the windowed run from that frame on.
#
# MEASURED 2026-10-17, and it is worth stating before anyone builds on this.
# The anchor does its job -- the state carries the RNG, the frame offset is
# right, and the replay's guest state is bit-for-bit the window's. What it does
# NOT do is stay bit-for-bit forever, and how long it lasts is NOT a frame count:
# measured at +9 past the anchor, a scripted sdb_throw capture had drifted by 85
# guest bytes and a played session had drifted by ZERO. Both were exact at +3.
# The onset depends on what the guest is doing when the two machines part
# company. So this fixes the frame-4 divergence long enough to be a real gate,
# but it does NOT make a replay a substitute for a capture, which is what that
# divergence costs. See tools/state/README.md for the whole measurement.
#
# What it also does not give you: everything BEFORE the anchor is still a
# different run, and our own mods put nothing in the save state
# (CycHostExtras has no savestate hook, and src/sdb_host_extras.c registers
# none), so the flicker mod's published sprite list starts empty at the anchor
# and takes a frame or two to rebuild.
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
#   SDB_CAMERA      camera for widescreen: follow | locked   (follow)
#                   locked pins the canvas to the court instead of to the game's
#                   camera, so the picture does not slide (research.md S28). It is
#                   written into state.toml alongside the aspect, because a capture
#                   that cannot be replayed from its own state file is not a
#                   capture. (The env var alone would work at run time -- the code
#                   reads it ahead of the option -- but not for the replay.)
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
#   SDB_CAPTURES    <from>-<to> for the SDB_TRACE_CAPTURES range, so the sdbw /
#                   sdbc / sdbv / sdbb / sdbp per-object lines land in trace.log
#                   for margin_track.py and dupes.py to audit.
#
#                     SET IT HERE, NOT WITH `export`. Those lines are gated on
#                     SDB_TRACE_CAPTURES ALONE -- not on SDB_TRACE -- so an
#                     exported range outlives the capture and every later run
#                     inherits it. `./build_and_run.sh` then floods the console
#                     with thousands of sdbw lines for a normal play session,
#                     which is what happened on 2026-10-17 after a capture
#                     instruction told the reader to export it.
#                     (default: the whole run, passed inline so the caller's
#                     environment is untouched)
#   SDB_VECTOR     replay a recorded `--input` schedule through the WINDOW
#                  instead of waiting for a human (default: off, play it
#                  yourself). The window is where a capture has to be made
#                  (AGENTS.md: a headless replay is a different machine), so an
#                  unattended capture has to be a WINDOWED one -- and this is
#                  also how the anchor is checked without a person in the loop.
#
#                      SDB_VECTOR=tas/vectors/sdb_throw.input \\
#                      SDB_ANCHOR=600 tools/play_record.sh anchor-check 900
#
#                  Unlike --virtual-pad this does NOT go through the bindings:
#                  cyc_host_window_replay writes the NES controller bytes
#                  directly, so Player1Source is irrelevant here. That is what
#                  makes it usable for a gate; a --virtual-pad replay needs
#                  Player1Source = 2 or the guest sees nothing at all.
#   SDB_CHEAT       codes to ARM now, ';'-joinable, repeatable  (default: none)
#                       SDB_CHEAT='043B:00;0403:00;03CB:00' ...
#                   Passed straight through as --cheat. This is the only way an
#                   unattended capture can carry a cheat, because the in-game
#                   Cheats menu (Escape) is a mouse-driven widget no script can
#                   drive -- so arming it here is not a convenience, it is the
#                   only door.
#   SDB_CHEATS      a .cht FILE to load instead of the default one  (default: none)
#                   Passed as --cheats, which SUPERSEDES any --cheat already
#                   given, so setting both does not mean both. Note the window
#                   loads cheats/Super Dodge Ball (USA).cht from the repo root
#                   on its own and every entry there ships disabled, so this
#                   knob loads a menu rather than arming anything.
#   SDB_NO_CHEATS   1 passes --no-cheats, dropping every loaded cheat  (default 0)
#                   Checked first of the three: it supersedes them, and a capture
#                   that passed --no-cheats AND --cheat would be asking for two
#                   opposite things with the engine's order deciding, not you.
#
#                   Beware what a cheat does NOT do: research.md S32 refutes the
#                   obvious reading of the P2 energy codes. Freezing them at 0
#                   HOLDS and the match PLAYS ON, because the match is a fixed
#                   ~37-second scripted sequence, not something a player can end
#                   early. A handicap, not a kill switch.
#   SDB_LAUNCHER    1 keeps the recomp-ui launcher; 0 skips it  (default 0)
#   SDB_UNCAPPED    1 runs as fast as the machine can    (default 0, frame-paced)
#   SDB_TCP         port for the host's debug server, so a tap can sample the
#                   RUNNING WINDOW's RAM (default: off unless SDB_ANCHOR is on).
#                   The windowed host has no --frame-log -- that is the headless
#                   host -- so capturing RAM where it was made means asking the
#                   live window over this port. Worth it whenever the question is
#                   about machine state: a headless replay of this vector is a
#                   DIFFERENT machine from frame 4 (see the replay note this
#                   script appends), so a diff taken there is a diff of the wrong
#                   thing.
#                       tools/cheats/ram_tap.py --port <port> --out tmp/<name>/ram.csv &
#   SDB_ANCHOR      frame to write state.bin at (default 600; 0 = off).
#                   600 is not arbitrary: the court appears at 412 and the input
#                   window opens at 594 (AGENTS.md), so it is the first frame
#                   from which input can do anything at all. Lower it to study a
#                   menu, higher it to shorten the anchored tail.
#                   The frame you ASK for is not the frame you GET: the window
#                   dispatches the request on a later host loop than the ping
#                   that preceded it. anchor.json holds the frame the server
#                   reported, and every later number is derived from that one.
#   SDB_ANCHOR_PROBE  frames after the anchor to take a SECOND save state at, so
#                   the anchor can be CHECKED rather than merely taken.
#                   DEFAULT 0 (off) -- a measured choice, not laziness.
#
#                   Two save states cannot be closer than about 6 frames apart on
#                   a 16.6 ms host loop: each is 449508 bytes and writing one
#                   costs the window 3-6 frames (the first save lands 3 late, the
#                   second 6). So a probe lands at +6 or beyond, and whether the
#                   machines still agree there is NOT predictable -- measured at
#                   +9, a scripted vector had drifted by 85 guest bytes and a
#                   played session by ZERO, both exact at +3. A check that
#                   reports drift every single time is not a gate, it is noise,
#                   and a capture that prints a red verdict by default teaches
#                   you to ignore it.
#
#                   Turn it on when the drift is the question:
#                     SDB_ANCHOR_PROBE=9 tools/play_record.sh name
#
#                   It is a state and not a RAM dump because a dump cannot be
#                   labelled honestly: `ping` then `read_ram` is two round trips
#                   inside a 16.6 ms frame, so the RAM belongs 2-3 frames past
#                   the label (measured: consistently 3). The first version of
#                   this did that and reported 160-460 differing RAM bytes at
#                   every frame, which reads as "different machine" and is
#                   actually a mislabelled sample. A save state REPLIES with the
#                   frame it saved at, so both frame numbers are authoritative.
#   SDB_ANCHOR_PORT  port for the anchor helper (default 9091).
#                   ONE CLIENT AT A TIME: the host's debug server holds a single
#                   connection (cyc_tcp.c: s_client), so this cannot share a port
#                   with a RAM tap. If SDB_TCP is set, that port is used and you
#                   get a warning; run the tap after the anchor has been taken,
#                   or give the anchor its own port here.
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
CAMERA="${SDB_CAMERA:-follow}"
SCANLINES="${SDB_SCANLINES:-262}"

case "$CAMERA" in
    follow|locked) ;;
    '')
        echo "play_record: SDB_CAMERA is empty; use follow or locked." >&2
        exit 2 ;;
    *)
        echo "play_record: SDB_CAMERA=$CAMERA is not a camera." >&2
        echo "             valid: follow, locked" >&2
        exit 2 ;;
esac

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
VECTOR="${SDB_VECTOR:-}"
CHEAT="${SDB_CHEAT:-}"
CHEATS="${SDB_CHEATS:-}"
NO_CHEATS="${SDB_NO_CHEATS:-0}"
SHOT_EVERY="${SDB_SHOT_EVERY:-1}"
SCALE="${SDB_SCALE:-1}"
LAUNCHER="${SDB_LAUNCHER:-0}"
UNCAPPED="${SDB_UNCAPPED:-0}"
TCP="${SDB_TCP:-}"

# The anchor. Note the `-` and not `:-` again, for the same reason as SDB_MODS:
# SDB_ANCHOR=0 has to mean "no state", and ${VAR:-default} fires on an empty
# string as well as an unset one, so it would silently capture a state anyway.
ANCHOR="${SDB_ANCHOR-600}"
ANCHOR_PROBE="${SDB_ANCHOR_PROBE-0}"
ANCHOR_PORT="${SDB_ANCHOR_PORT:-9091}"

for kv in "SDB_ANCHOR:$ANCHOR" "SDB_ANCHOR_PROBE:$ANCHOR_PROBE" "SDB_ANCHOR_PORT:$ANCHOR_PORT"; do
    case "${kv#*:}" in
        ''|*[!0-9]*)
            echo "play_record: ${kv%%:*} wants a whole number, got '${kv#*:}'" >&2
            exit 2 ;;
    esac
done

# One debug server, one client at a time (cyc_tcp.c). Sharing a port with a RAM
# tap would have them fight over the socket, and the loser is whichever one the
# project notices least -- so say it rather than let it look like a flaky tap.
if [ "$ANCHOR" != "0" ] && [ -n "$TCP" ] && [ "$TCP" != "$ANCHOR_PORT" ]; then
    echo "    note: SDB_TCP=$TCP is also the anchor's port. The host's debug"
    echo "          server holds ONE connection, so a RAM tap cannot run"
    echo "          alongside the anchor. Start the tap after the anchor is"
    echo "          taken, or set SDB_ANCHOR_PORT to something else."
fi
# A fixed-length run knows its own length, so an anchor past the end is caught
# here rather than reported as a failure after a capture that was perfectly
# good. UNLIMITED cannot know, so it is left to the helper's exit code.
# This runs BEFORE the port defaulting below: zeroing ANCHOR after TCP had been
# assigned leaves a listening socket and a "start a RAM tap here" prompt for a
# run that has no anchor to take.
if [ "$ANCHOR" != "0" ] && [ "$UNLIMITED" != "1" ] && [ "$ANCHOR" -ge "$FRAMES" ]; then
    echo "play_record: SDB_ANCHOR=$ANCHOR is past the end of a $FRAMES-frame run," >&2
    echo "             so there would be no anchor. Lower it, raise the frame" >&2
    echo "             count, or SDB_ANCHOR=0 to stop asking." >&2
    ANCHOR=0
fi

# The anchor needs the debug server, so it gets a port. Only then: with
# SDB_ANCHOR=0 the window must not open a port it never used to, or "no anchor"
# quietly becomes "a listening socket nobody is using".
if [ "$ANCHOR" != "0" ] && [ -z "$TCP" ]; then TCP="$ANCHOR_PORT"; fi

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
# The anchor files are in the same line for the same reason and more sharply: a
# leftover state.bin replays as this run's anchor and answers questions about
# a session that is gone.
rm -f "$OUT"/w_*.png "$OUT"/trace.log "$OUT"/repro.input \
      "$OUT"/state.bin "$OUT"/probe.bin "$OUT"/replay.bin \
      "$OUT"/anchor.json "$OUT"/anchor.log

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
# At 1x the same shot is 300 KB and every frame is affordable.
#
# THE CONFIG IS WRITTEN FROM SCRATCH, NOT DERIVED. This used to be
#   sed ... build-cycle/config.ini > "$OUT/config.ini"
# which pinned three keys and inherited every other one -- and
# build-cycle/config.ini is owned by the launcher and rewritten by it and by the
# in-game menu. So a capture silently depended on shared mutable state that
# nothing in the capture recorded: toggle LinearFilter in the launcher and every
# LATER capture is a different picture, for a reason that has nothing to do with
# the game. The whole point of SDB_SCALE / SDB_MODS / SDB_ASPECT is that the
# capture is a function of its parameters; deriving the pins from a file the
# launcher owns puts that back.
#
# So every key the capture depends on is pinned here, explicitly, and nothing is
# inherited. What is NOT listed -- Player1Source and the bindings -- comes from
# cyc_bindings_default (cyc_input.c), which is already the arrangement the old
# derivation was preserving: player 1 on the KEYBOARD, player 2 on the
# controller, A=Z B=X Select=\ Start=Return arrows. Losing Player1Source used to
# be the trap here -- a config without it records the whole session on port 2
# and the guest sees nothing -- and that cannot happen now, because the default
# IS 1.
#
# IntegerScale and LinearFilter are pinned too, and they were the live risk:
# LinearFilter changes the captured image directly, and neither was pinned
# before. HdPackEnabled is off so a texture pack found on the machine cannot
# change the capture either.
#
# The file you find in tmp/<name>/ afterwards is LONGER than this: the host saves
# the whole settings set on exit, so the pins sit at the top and every default
# the host filled in is spelled out below them. That is worth having -- the
# capture then records the complete machine it was made on rather than the six
# keys somebody remembered to pin.
cat > "$OUT/config.ini" <<EOF
[Display]
WindowScale = $SCALE
Fullscreen = 0
IntegerScale = 0
LinearFilter = 0
VSync = 0
HdPackEnabled = 0
EOF

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
                    echo "camera = \"$CAMERA\""
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

# The capture range is passed INLINE and never exported: see SDB_CAPTURES above
# for what an inherited one costs.
CAPTURES="${SDB_CAPTURES:-0-$([ "$UNLIMITED" = "1" ] && echo 20000 || echo "$FRAMES")}"

if [ "$UNLIMITED" = "1" ]; then
    echo "=== $NAME: UNLIMITED (no --exit-after), mods: $MODS_ON ==="
else
    echo "=== $NAME: $FRAMES frames, mods: $MODS_ON ==="
fi
# An `&&` on its own line would abort the script under `set -e` when false, so
# this is an `if`.
if [ "$WS_ON" = "true" ]; then
    echo "    aspect: $ASPECT"
    echo "    camera: $CAMERA"
fi
echo "    vector  -> $OUT/repro.input"
if [ "$ANCHOR" != "0" ]; then
    if [ "$ANCHOR_PROBE" = "0" ]; then
        echo "    anchor  -> $OUT/state.bin at frame $ANCHOR (no probe: SDB_ANCHOR_PROBE=0)"
    else
        echo "    anchor  -> $OUT/state.bin at frame $ANCHOR, probe +$ANCHOR_PROBE"
    fi
fi
echo "    captures-> $CAPTURES (sdbw/sdbc/sdbv/sdbb/sdbp in trace.log)"
echo "    trace   -> $OUT/trace.log"
echo "    shots   -> $OUT/w_<frame>.png (every $SHOT_EVERY)"
if [ "$ANCHOR" != "0" ]; then
    echo "    tcp     -> 127.0.0.1:$TCP (the anchor helper; it connects and waits)"
elif [ -n "$TCP" ]; then
    echo "    tcp     -> 127.0.0.1:$TCP (a RAM tap can sample this live window)"
    echo
    echo "Start the tap in another terminal, in either order -- it waits for the port:"
    echo "  python3 tools/cheats/ram_tap.py --port $TCP --out $OUT/ram.csv &"
fi
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
if [ -n "$TCP" ]; then ARGS+=( --tcp "$TCP" ); fi
# --window-input, not --virtual-pad: the first writes the NES controller bytes
# through cyc_host_window_replay, so it needs no bindings and no physical pad.
# The second injects SDL events and would be dropped by a keyboard-bound port 1.
if [ -n "$VECTOR" ]; then
    if [ ! -f "$VECTOR" ]; then
        echo "play_record: SDB_VECTOR names no file: $VECTOR" >&2
        exit 2
    fi
    ARGS+=( --window-input "$VECTOR" )
    echo "    vector  -> $VECTOR (played through the window, unattended)"
fi

if [ "$NO_CHEATS" = "1" ]; then
    ARGS+=( --no-cheats ); CHEAT_FLAG="--no-cheats"
elif [ -n "$CHEATS" ]; then
    if [ ! -f "$CHEATS" ]; then
        echo "play_record: SDB_CHEATS names no file: $CHEATS" >&2
        exit 2
    fi
    ARGS+=( --cheats "$CHEATS" ); CHEAT_FLAG="--cheats $CHEATS"
elif [ -n "$CHEAT" ]; then
    ARGS+=( --cheat "$CHEAT" ); CHEAT_FLAG="--cheat $CHEAT"
else
    CHEAT_FLAG=""
fi
[ -n "$CHEAT_FLAG" ] && echo "    cheats  -> $CHEAT_FLAG"

# The anchor helper starts FIRST and waits for the port, so the two do not have
# to be started in a particular order -- and starting it first is what makes the
# anchor land early enough to be worth having. It holds the host's single TCP
# client slot until it has what it asked for, which is why it cannot share a
# port with a RAM tap.
ANCHOR_PID=""
if [ "$ANCHOR" != "0" ]; then
    python3 tools/state/state_anchor.py --port "$TCP" --frame "$ANCHOR" \
        --probe "$ANCHOR_PROBE" --out "$OUT" > "$OUT/anchor.log" 2>&1 &
    ANCHOR_PID=$!
    # If the script dies now the helper would outlive it and keep the port, so
    # the trap is armed before the wait rather than after it.
    trap 'kill "$ANCHOR_PID" 2>/dev/null || true' EXIT
fi

set +e
SDB_TRACE=1 SDB_TRACE_CAPTURES="$CAPTURES" \
NESRECOMP_NO_LAUNCHER=$( [ "$LAUNCHER" = "1" ] && echo 0 || echo 1 ) \
"$GAME" "${ARGS[@]}" > "$OUT/trace.log" 2>&1
RC=$?
set -e

# Collect the anchor. `wait` reports the helper's exit status, which is 0/1/2/3
# and each means something different (see state_anchor.py) -- so it is captured,
# not tested for truthiness: 3 is a state that exists and was never verified,
# which is not the same as no state, and collapsing the two is how a capture
# ends up quoting a replay that cannot be trusted.
ANCHOR_RC=0
A_FRAME=""; P_FRAME=""; P_MEM=""; STATE_BYTES=""; P_GAP=""
if [ -n "$ANCHOR_PID" ]; then
    if wait "$ANCHOR_PID"; then ANCHOR_RC=0; else ANCHOR_RC=$?; fi
    trap - EXIT
    ANCHOR_PID=""
    if [ -s "$OUT/anchor.json" ]; then
        # anchor.json is written by our own tool, and python3 is already a
        # dependency of everything here, so read it properly rather than
        # grepping for a number out of a JSON document.
        while IFS='=' read -r k v; do
            case "$k" in
                anchor_frame)   A_FRAME="$v" ;;
                probe_frame)    P_FRAME="$v" ;;
                probe_mem_frame) P_MEM="$v" ;;
                probe_gap)      P_GAP="$v" ;;
                state_bytes)    STATE_BYTES="$v" ;;
            esac
        done <<EOF
$(python3 - "$OUT/anchor.json" <<'PY'
import json, sys
try:
    rec = json.load(open(sys.argv[1]))
except Exception as e:
    print(f"anchor.json unreadable: {e}", file=sys.stderr)
    sys.exit(0)
for k in ("anchor_frame", "probe_frame", "probe_mem_frame", "probe_gap", "state_bytes"):
    if rec.get(k) is not None:
        print(f"{k}={rec[k]}")
PY
)
EOF
    fi
    if [ ! -s "$OUT/state.bin" ]; then
        A_FRAME=""; P_FRAME=""; P_MEM=""
    fi
fi
ANCHOR_OK=0
[ -n "$A_FRAME" ] && ANCHOR_OK=1

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

# A cheat that does not reach the replay is worse than no cheat: the capture and
# the run measured from it would be different machines for a reason nothing in
# the printed command shows.
CHEAT_CONT=""
[ -n "$CHEAT_FLAG" ] && CHEAT_CONT=" $CHEAT_FLAG"

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
#       --mods-root $OUT/mods $WS_FLAG$EXTRA_FLAG$CHEAT_CONT \\
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

if [ "$ANCHOR_OK" = "1" ]; then
if [ -n "$P_MEM" ]; then
cat >> "$OUT/repro.input" <<EOF
#
# --- replay FROM THE ANCHOR (the reproducible one) ---
#   anchor frame $A_FRAME   probe frame $P_FRAME, $P_GAP frames later
#
#   cd "$PWD" && SDB_TRACE=1 ./build-cycle/nes_game "$ROM" \\
#       --load-state $OUT/state.bin --frames $REC_FRAMES \
#       --input $OUT/repro.input --ram-init zeros \\
#       --mods-root $OUT/mods $WS_FLAG$EXTRA_FLAG$CHEAT_CONT \
#       --save-state $P_MEM:$OUT/replay.bin
#
#   python3 tools/state/state_check.py $OUT
#
# --save-state is $P_MEM: the window probe frame $P_FRAME MINUS ONE. The window's
# TCP counter counts frames COMPLETED, while --save-state M saves after frame M
# has RAN. anchor.json records that conversion so it happens once, in one place.
#
# What the comparison will say: the guest (CPU, RAM, PPU, picture, the mods'
# records) is byte-identical at the anchor and at +3 frames from it, and the two
# machines part company a few frames after that -- WHERE is content-dependent,
# not a fixed horizon (measured at +9: 85 guest bytes of drift on a scripted
# vector, ZERO on a played session). At this gap state_check.py will normally
# report DRIFTED. That is the windowed and headless machines parting company
# again (AGENTS.md), NOT a broken anchor: a save state IS the machine, so the load
# put the replay exactly on this run's state. Two 450 KB states cannot be taken
# closer than about 6 frames apart on a 16.6 ms host loop, which is why the probe
# cannot reach the clean window at all.
#
# --frames is still the TOTAL ($REC_FRAMES), not the frames after the anchor: the
# save state carries the host's frame counter and --load-state continues from it
# (cyc_host.c: s_loaded_frame = cyc_ring_frame at save time).
#
# What the anchor does NOT buy: a state cannot undo a divergence that already
# happened, so everything before frame $A_FRAME is still a different run. And our
# own mods put nothing in the save state -- CycHostExtras has no savestate hook
# and src/sdb_host_extras.c registers none -- so the flicker mod's published
# sprite list arrives empty and rebuilds over the first frame or two.
EOF
else
cat >> "$OUT/repro.input" <<EOF
#
# --- replay FROM THE ANCHOR ---
#   anchor frame $A_FRAME (SDB_ANCHOR_PROBE was 0, so there is no probe state
#   and nothing to check the replay against -- see tools/state/README.md)
#
#   cd "$PWD" && SDB_TRACE=1 ./build-cycle/nes_game "$ROM" \\
#       --load-state $OUT/state.bin --frames $REC_FRAMES \
#       --input $OUT/repro.input --ram-init zeros \\
#       --mods-root $OUT/mods $WS_FLAG$EXTRA_FLAG
#
# --frames is still the TOTAL ($REC_FRAMES), not the frames after the anchor: the
# save state carries the host's frame counter and --load-state continues from it.
# A state cannot undo a divergence that already happened, so everything before
# frame $A_FRAME is still a different run.
EOF
fi
fi

echo
echo "=== recorded $REC_FRAMES frames, $REC_STEPS input steps (exit $RC) ==="
grep -E "^(widescreen|margin sprites):" "$OUT/trace.log" || true
echo
echo "Replay it with:"
echo "  SDB_TRACE=1 ./build-cycle/nes_game \"$ROM\" \\"
echo "      --frames $REC_FRAMES --input $OUT/repro.input --ram-init zeros \\"
echo "      --mods-root $OUT/mods $WS_FLAG$EXTRA_FLAG$CHEAT_CONT \\"
echo "      --present-out $OUT/r_ --present-every 1 2> $OUT/replay.log"

if [ "$ANCHOR_OK" = "1" ]; then
    echo
    echo "Anchor: frame $A_FRAME, $STATE_BYTES bytes -> $OUT/state.bin"
    echo "  From frame $A_FRAME on, a replay that loads it starts from EXACTLY"
    echo "  this run's machine -- the game's RNG included. Reproduce it with:"
    echo
    echo "  SDB_TRACE=1 ./build-cycle/nes_game \"$ROM\" \\"
    echo "      --load-state $OUT/state.bin --frames $REC_FRAMES \\"
    echo "      --input $OUT/repro.input --ram-init zeros \\"
    echo "      --mods-root $OUT/mods $WS_FLAG$EXTRA_FLAG$CHEAT_CONT \\"
    if [ -n "$P_MEM" ]; then
        echo "      --save-state $P_MEM:$OUT/replay.bin \\"
    fi
    echo "      --present-out $OUT/r_ --present-every 1"
    echo
    echo "  --frames is still the TOTAL ($REC_FRAMES), not the frames after the anchor:"
    echo "  the save state carries the host's frame counter and --load-state continues"
    echo "  from it (cyc_host.c: s_loaded_frame = cyc_ring_frame at save)."
    if [ -n "$P_MEM" ]; then
        echo
        echo "  That command also writes the replay's own state to $OUT/replay.bin."
        echo "  This capture holds the WINDOW's state for that same frame (frame"
        echo "  $P_FRAME, $P_GAP frames past the anchor), so the two can be compared:"
        echo
        echo "    python3 tools/state/state_check.py $OUT"
        echo
        echo "  ($P_MEM is the window probe frame $P_FRAME minus one: the window counts"
        echo "   frames COMPLETED, the headless host saves after a frame RAN."
        echo "   state_check.py reads that out of anchor.json rather than repeating it.)"
        echo
        echo "  Expect DRIFTED at this gap, but not because the gap is over any fixed"
        echo "  horizon: the two hosts part company a few frames past the anchor and"
        echo "  WHERE is content-dependent -- measured at +9, a scripted vector had"
        echo "  drifted by 85 guest bytes and a played session by ZERO, both exact at"
        echo "  +3. Two 450 KB states cannot be taken closer than about 6 frames apart"
        echo "  on a 16.6 ms host loop, so a probe cannot reach the clean window at"
        echo "  all. The state is sound; the drift is the open engine question, and"
        echo "  this tool says which of the two you are"
        echo "  looking at rather than collapsing them into one red line."
    fi
elif [ "$ANCHOR" != "0" ]; then
    echo
    echo "Anchor: NONE (helper exit $ANCHOR_RC) -- see $OUT/anchor.log"
    case "$ANCHOR_RC" in
        1) cat <<'EOT'
  The debug port never opened. If SDB_TCP named a port something else was
  holding it, or the run died before the server came up.
EOT
            ;;
        2) cat <<EOT
  The window ended before frame $ANCHOR, so nothing was saved. The capture
  itself is fine -- vector, trace and shots are all present and usable -- but it
  replays from power-on, which is the run that parts company with the window at
  frame 4. Lower SDB_ANCHOR toward the start, or run the session longer.
EOT
            ;;
        3) cat <<EOT
  The state WAS saved but the run ended before the +$ANCHOR_PROBE-frame probe,
  so there is nothing to check a replay against. The state is usable; it is just
  unverified. Re-capture with the probe frame inside the run.
EOT
            ;;
        *) echo "  The helper exited $ANCHOR_RC, which it should not. That is a bug" \
                  " in tools/state/state_anchor.py; $OUT/anchor.log has its last words." ;;
    esac
    echo "  SDB_ANCHOR=0 stops asking for one."
fi

#!/usr/bin/env python3
"""margin_track.py - per-sprite audit of the widescreen margin sprites.

`SDB_TRACE_CAPTURES=<from>-<to>` prints one `sdbw` line per presented frame
listing every margin sprite the compositor painted, with the object index and
the screen column it was placed at. That line is the ground truth for "what
ended up in the margins", and this script reads it the way the eye cannot: one
row per object, one column per frame, so a sprite that vanishes, jumps, doubles
up or changes size is a number rather than something to spot in a screenshot.

    SDB_TRACE_CAPTURES=500-1200 ./build-cycle/nes_game ROM --frames 1200 \\
        --input tas/vectors/sdb_sweep.input --ram-init zeros \\
        --widescreen 16:9 --overscan --present-out tmp/x/y_ --present-every 1 \\
        > /dev/null 2> tmp/x/trace.log
    python3 tools/widescreen/margin_track.py tmp/x/trace.log
    python3 tools/widescreen/margin_track.py tmp/x/trace.log --frames 600:660

WHAT IT CHECKS, and why each one is a real defect rather than a style rule:

  gone       A frame where the object is between draws but its display record is
             still live and still off-screen. This is the blink: an object can
             be missing from `$067C` for most of a match (measured on the sweep:
             `$8231` fires for record 8 once per 100 frames, and record 2 four
             times), so nothing measured in emissions survives as a liveness
             test. The record is the test.

  shift      The object's column moving further in one frame than the camera
             moved plus its own walk. A sprite in the margin should track the
             world; a jump means it was re-placed from a different build.

  ghost      Two parts of the same object on the same row within one pixel of
             each other. That is one sprite drawn twice a pixel apart, which
             reads as "shifted a pixel" or "slightly wider than it should be".
             It is what a stale part from the previous emission looks like: the
             part is keyed by (offset, row), and a 1 px animation offset moves
             the key instead of replacing it.

  part       A frame where the object is wholly off-screen (every part outside
             the viewport) and holds fewer parts than it held a moment ago. A
             character is eight sprites; holding four of them is half a player,
             which is the "only a sliver is kept" symptom.

WHAT IT DELIBERATELY DOES NOT CHECK, because it cannot be made sound here: a
margin sprite drawn on top of the machine's own copy of the same player. The
obvious test -- match tile and row against OAM -- reports thousands of false
hits, because Super Dodge Ball gives every player the same tiles, and the
apparently exact version (match the OAM *slot* the emitter was about to use)
cannot work either: $0617 only advances for sprites the game actually draws, so
a run of culled sprites shares one cursor value and the slot identifies nothing.

That property is proved a different way and the proof is in AGENTS.md: the
compositor clips its blit to the columns the machine does not own, so comparing
a margin-sprite run against a `--margin-sprites 0` run must show **0 differing
pixels** across the machine's 256 columns on every frame. If a margin sprite
ever landed on the machine's picture, that number stops being zero.

Output is one summary table per object plus a list of every anomaly with its
frame, so a report can point at a number instead of "somewhere around there".
"""
import argparse
import collections
import re
import struct
import sys

REC_BASE = 0x0618
REC_STRIDE = 7
CAMERA = 0x00C3

LINE = re.compile(r'^sdbw f=(\d+) x0=(\d+):(.*) n=(\d+)$')
ENTRY = re.compile(r'\[o(\d+):(-?\d+),(\d+),t([0-9A-F]{2})\]')

# The PPU clips its leftmost 8 screen columns; with overscan revealing them they
# belong to the margin, so a part at 0..7 is one only this feature can draw.
CLIP = 8
VIEW_LO, VIEW_HI = 0, 251


def read_trace(path):
    """{frame: {object: [(screen column, row, tile), ...]}}"""
    out = {}
    for line in open(path):
        m = LINE.match(line.strip())
        if not m:
            continue
        per = collections.defaultdict(list)
        for o, x, y, t in ENTRY.findall(m.group(3)):
            per[int(o)].append((int(x), int(y), t))
        out[int(m.group(1))] = dict(per)
    return out


def read_records(path, lo, hi):
    """{frame: {index: (state, selector, screen X)}} from a --frame-log."""
    out = {}
    with open(path, "rb") as f:
        if f.read(8) != b"CYCFRAME":
            raise SystemExit(f"{path}: not a CYCFRAME log")
        f.read(4)
        while True:
            head = f.read(36)
            if len(head) < 36:
                return out
            (fr, _, _, l_ram, l_ciram, l_cart, l_chr, l_pic, l_aud) = struct.unpack("<9I", head)
            f.read(32)
            f.read(256)                       # OAM: not used, see the note below
            ram = f.read(l_ram)
            f.read(l_ciram); f.read(l_cart); f.read(l_chr); f.read(l_pic); f.read(l_aud)
            if not (lo <= fr <= hi):
                continue
            cam = ram[CAMERA] | (ram[CAMERA + 1] << 8)
            recs = {}
            for i in range(13):
                r = ram[REC_BASE + REC_STRIDE * i: REC_BASE + REC_STRIDE * i + REC_STRIDE]
                recs[i] = (r[0], r[1], (r[3] | (r[4] << 8)) - cam)
            out[fr] = recs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", help="stderr log from SDB_TRACE_CAPTURES=<from>-<to>")
    ap.add_argument("--records", metavar="LOG",
                    help="a --frame-log of the same run, for the record cross-check")
    ap.add_argument("--frames", metavar="LO:HI", help="restrict the audit to a range")
    ap.add_argument("--walk", type=int, default=8,
                    help="pixels an object may move per frame before it counts "
                         "as a jump (default 8, about one camera step)")
    ap.add_argument("--reach", type=int, default=16,
                    help="how far past the viewport edge an anchor must be before "
                         "a missing object counts as a blink (default 16, a "
                         "character's width)")
    args = ap.parse_args()

    drawn = read_trace(args.trace)
    if not drawn:
        raise SystemExit("no `sdbw` lines in that log")
    lo, hi = min(drawn), max(drawn)
    if args.frames:
        lo, hi = (int(v) for v in args.frames.split(":"))
    frames = [f for f in sorted(drawn) if lo <= f <= hi]
    recs = read_records(args.records, lo, hi) if args.records else {}

    objects = sorted({o for f in frames for o in drawn[f]})
    print(f"frames {lo}..{hi} ({len(frames)}), objects {objects}")

    anomalies = []
    for o in objects:
        present = [f for f in frames if o in drawn[f]]
        parts = [len(drawn[f][o]) for f in present]
        cols = [p[0] for f in present for p in drawn[f][o]]
        print(f"\nobj {o}: drawn on {len(present)}/{len(frames)} frames"
              f"  column {min(cols)}..{max(cols)}"
              f"  parts min {min(parts)} max {max(parts)}")

        # gone: live record, still off-screen, nothing painted
        if recs:
            for f in frames:
                rec = recs.get(f, {}).get(o)
                if not rec or rec[0] == 0:
                    continue
                screen_x = rec[2]
                if VIEW_LO <= screen_x <= VIEW_HI:
                    continue                      # the machine owns it
                # The record holds the object's ANCHOR, and a part sits up to a
                # character's width either side of it, so an anchor just outside
                # the viewport can still have every one of its parts inside it.
                # The machine then draws the object and the margin correctly has
                # nothing to say; reporting that as a blink is a false positive.
                # Only judge an anchor clear of the edge by more than a character.
                if (screen_x > VIEW_LO - args.reach) and (screen_x < VIEW_HI + args.reach):
                    continue
                if o not in drawn[f]:
                    anomalies.append(("gone", f, o,
                                       f"record live at screen X={screen_x}, nothing drawn"))
                elif len(drawn[f][o]) < min(parts):
                    anomalies.append(("part", f, o,
                                       f"{len(drawn[f][o])} of {min(parts)} parts, "
                                       f"record live at screen X={screen_x}"))

        # shift and ghost
        prev = None
        for f in frames:
            cur = drawn[f].get(o)
            if not cur:
                prev = None
                continue
            # The object's position is its LEFTMOST part's column. Comparing
            # every part against every part would report the 8 px between a
            # character's own two columns, and would double-report every build
            # that changed which of its parts the machine drew.
            left = min(p[0] for p in cur)
            if prev is not None and recs:
                # How far the object's anchor moved is the ONLY movement a margin
                # sprite can legitimately show, since the part offsets are a
                # property of the object's animation and not of the frame.
                #
                # Compared one frame back, and that is measured, not assumed: the
                # compositor reads the record after the frame it is presenting has
                # been rendered, so on the sweep its column answers to the record
                # as of the previous frame. Taking it as current-time reports
                # every frame of a pan as a jump -- object 10 moves 9 px at frame
                # 759 and the margin shows those 9 px at 760, which is correct.
                a = recs.get(prev[0] - 1, {}).get(o)
                b = recs.get(f - 1, {}).get(o)
                if a and b:
                    want = prev[1] + (b[2] - a[2])
                    if abs(left - want) > args.walk:
                        anomalies.append(("shift", f, o,
                                           f"leftmost column {left}, expected {want} "
                                           f"(anchor moved {b[2] - a[2]})"))
            elif prev is not None and abs(left - prev[1]) > args.walk:
                anomalies.append(("shift", f, o,
                                   f"leftmost column moved {abs(left - prev[1])} px "
                                   f"in one frame"))
            prev = (f, left)
            byrow = collections.defaultdict(list)
            for x, y, _t in cur:
                byrow[y].append(x)
            for y, xs in byrow.items():
                xs = sorted(xs)
                for a, b in zip(xs, xs[1:]):
                    if b - a < 2:
                        anomalies.append(("ghost", f, o,
                                           f"two parts on row {y} at columns {a} and {b}"))

    print()
    if not anomalies:
        print("no anomalies")
        return 0
    by_kind = collections.Counter(a[0] for a in anomalies)
    print(f"{len(anomalies)} anomalies: " +
          ", ".join(f"{k} {v}" for k, v in sorted(by_kind.items())))
    for kind, f, o, msg in anomalies[:80]:
        print(f"  {kind:6s} frame {f:5d} obj {o:2d}  {msg}")
    if len(anomalies) > 80:
        print(f"  ... {len(anomalies) - 80} more")
    return 1


if __name__ == "__main__":
    sys.exit(main())

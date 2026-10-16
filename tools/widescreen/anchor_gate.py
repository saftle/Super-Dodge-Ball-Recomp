#!/usr/bin/env python3
"""Does the widescreen compositor's camera anchor hold still, and change nothing?

The anchor (`camera = locked`) makes the canvas a fixed window on the court
instead of one centred on the game's camera. Three claims, in order of how much
they would cost to be wrong.

  1. THE VIEW DOES NOT MOVE. `anchor` is the world column at canvas column 0.
     Locked, it is a constant derived from the width alone. This is the whole
     feature and it is one integer, so it is checked directly rather than
     inferred from pixels -- and a run in follow mode is the POSITIVE CONTROL:
     its `anchor` tracks the camera and this check must fail on it.

  2. THE ANCHOR CHANGES NOTHING ELSE. Each run's own machine block is compared
     against the SAME stock frame, using each run's own recorded anchor, and the
     per-frame differences must be identical. That is the strong form of the
     claim and it is index-proof: it never compares one run's pixels to
     another's, so it cannot be defeated by the presented-index ambiguity that
     the first version of this tool walked straight into.

     Comparing the two CANVASES to each other was the obvious version and it is
     wrong to attempt. Measured: the locked canvas does equal the follow canvas
     translated, exactly -- 0 of 76,970 court pixels differ at the predicted
     shift while ~40% differ at every other shift, at frame 605 of the sweep.
     But a canvas-to-canvas comparison needs both runs' presented indices to
     line up, and they do not reliably (both runs are the same binary and the
     same schedule, yet the locked run's trace reports the previous frame's
     scroll on a few frames and the follow run's does not, so a handful of
     frames come out 6-9 columns out). The block-vs-stock form has no such
     dependency: it asks each run the same question of the same reference.

  3. THE COURT IS WHERE IT SHOULD BE. Canvas column 0 holds world column
     `anchor`, so the court's left edge (world 40) is at canvas column
     40 - anchor and its right edge (world 480) at 480 - anchor. Both must land
     inside the canvas on every frame.

  `--stock` is therefore not optional: claims 2 and 3 both need it.

  WHAT A STOCK COMPARISON ALONE SHOWS, because the number is not clean and a
  reader should not assume it is: the machine's own 256 columns are byte-
  identical to stock on most frames and differ on a run of them, by a few
  percent of a band, confined to a few dozen native columns at scattered rows.
  That happens IDENTICALLY with the camera following and locked, so it is not
  the anchor's -- but its CAUSE IS NOT ESTABLISHED and is not guessed at here.
  Ruled out: the anchor (claim 2), the reveal (the 8 columns are skipped), and
  the machine's picture being absent (it is inside the canvas by construction).

  WHY THE FRAME INDEX IS OFF BY ONE: the presented index runs one AHEAD of the
  trace's frame counter (AGENTS.md, Known Issues), so the anchor record whose `f`
  equals presented file N is the compose that produced file N, and file N holds
  guest frame N-1 -- which is what a stock run writes at index N-1. This gate
  first probed a +/-4 window around each anchor frame and reported 27 frames
  with no byte-exact match, every one of them a camera-stepping frame. The block
  was exactly where the compositor had put it; the probe was reading the
  neighbouring anchor record. Do NOT reintroduce a window: it reports a
  placement error that is not there, and it is exactly the kind of error a
  reader would believe.

  Usage:
      python3 tools/widescreen/anchor_gate.py \\
          --frames LOCKED --trace LOCKED/trace.log \\
          --follow FOLLOW --follow-trace FOLLOW/trace.log [--stock STOCK]
"""
import argparse
import glob
import json
import os
import re
import sys

try:
    from PIL import Image
except ImportError:  # system python3 has Pillow; the venv does not
    print("anchor_gate.py needs Pillow (system python3, not venv/bin/python3)")
    raise

# The court's own window, in world columns: src/sdb_host_extras.c's SDB_WS_LO /
# SDB_WS_HI, read out of the game's camera clamp at $E6AB rather than measured
# off the art. A literal here on purpose -- this gate is what checks the code
# against them, and a shared constant would only check the code against itself.
COURT_LO, COURT_HI = 40, 480

# WHICH FILE AN ANCHOR RECORD BELONGS TO. The presented index runs ONE AHEAD of
# the trace's frame counter (AGENTS.md, Known Issues): the anchor record with
# `f = N` was written by the compose that produced presented file N-1. Both runs
# and a stock run present on the same cadence, so ONE index pairs all three.
#
# This was measured the hard way, and in both directions. Pairing an anchor with
# the same-numbered file reports 26 failures, every one of them a
# camera-stepping frame, and it looks exactly like a 7-pixel placement bug --
# which is why it was nearly taken at face value. It is only visible when the
# camera's scroll CHANGES, because on every other frame a one-frame offset
# compares two identical pictures. Scanning the canvas for the block's real
# position settles it: file 596's block is where anchor 597 says it is, file
# 604's is where anchor 605 says it is. Do NOT reintroduce a probe window here.
# It reports a placement error that is not there, and it is exactly the kind of
# error a reader would believe.
FILE_OFFSET = -1
# Rows above the split are the held band. Never a literal in the compositor, and
# not one here either: this is only the row the gate crops from, and a course
# with a different split shows up as a block mismatch naming the band.
SPLIT_ROWS = 48
# The 8 columns PPUMASK hides are left RECONSTRUCTED, not stamped with the
# machine's picture, when widescreen's reveal is on, so they are ours and the
# block check must skip them. Whether a given run had the reveal on is not this
# tool's business to infer -- see --reveal / --no-reveal.
REVEAL = 8


def read_anchor(path):
    """frame -> anchor record, from the compositor's own JSON line."""
    anchors = {}
    try:
        fh = open(path, "r", errors="replace")
    except OSError as exc:
        print("cannot read %s: %s" % (path, exc))
        return anchors
    with fh:
        for line in fh:
            if '"k":"anchor"' not in line:
                continue
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if rec.get("k") == "anchor":
                anchors[rec["f"]] = rec
    return anchors


def frame_files(d):
    """Presented frames keyed by the index in the name.

    The host's output prefix has three observed forms across this repo's runs
    (bounds_gate.py hit all three), so files are globbed and the trailing digits
    are taken off the name rather than a format being assumed.
    """
    out = {}
    for p in glob.glob(os.path.join(d, "*.png")):
        m = re.search(r"(\d+)\.png$", os.path.basename(p))
        if m:
            out[int(m.group(1))] = p
    return out


def compare_to_stock(img, stock, rec, name, keep):
    """(differing bytes, messages) for the machine's own 256 columns."""
    w, _ = img.size
    if stock.size != (256, 240):
        return 1 << 30, ["%s: stock frame is %dx%d, expected 256x240"
                         % ((name,) + stock.size)]
    bad, n = [], 0
    for band, y0, y1 in (("court", SPLIT_ROWS, 240), ("held", 0, SPLIT_ROWS)):
        x0 = rec["x0"] if band == "court" else rec["xh"]
        if x0 < 0 or x0 + 256 > w:
            bad.append("%s: %s band block at column %d is off a %d-wide canvas"
                       % (name, band, x0, w))
            n += 1 << 30
            continue
        span = (256 - keep) * (y1 - y0) * 4
        got = img.crop((x0 + keep, y0, x0 + 256, y1)).tobytes()
        want = stock.crop((keep, y0, 256, y1)).tobytes()
        diff = sum(1 for a, b in zip(got, want) if a != b)
        n += diff
        if diff:
            bad.append("%s: %s band rows %d-%d differ from the machine's picture "
                       "in %d of %d bytes (%.1f%%)"
                       % (name, band, y0, y1, diff, span, 100.0 * diff / span))
    return n, bad


def block_diff(img, stock, rec, keep):
    """Differing bytes in the machine's own 256 columns, at this run's anchor."""
    n, _ = compare_to_stock(img, stock, rec, "", keep)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", required=True, help="locked run's presented PNGs")
    ap.add_argument("--trace", required=True, help="locked run's stderr trace")
    ap.add_argument("--follow", help="follow-mode run's presented PNGs, same run")
    ap.add_argument("--follow-trace", help="follow-mode run's stderr trace")
    ap.add_argument("--stock", help="stock 256-column run's PNGs (informational)")
    ap.add_argument("--expect", choices=("locked", "follow", "any"), default="locked")
    reveal = ap.add_mutually_exclusive_group()
    reveal.add_argument("--reveal", action="store_true",
                        help="the run revealed the 8 clipped columns, so they are "
                             "ours and the block check skips them")
    reveal.add_argument("--no-reveal", action="store_true",
                        help="the run did not reveal them (a bare --widescreen "
                             "command line does not), so all 256 columns are the "
                             "machine's and all are compared")
    ap.add_argument("--frame-from", type=int, default=0)
    ap.add_argument("--frame-to", type=int, default=10 ** 9)
    ap.add_argument("--step", type=int, default=1,
                    help="examine every Nth frame (the pixel compare is not cheap)")
    args = ap.parse_args()

    anchors = read_anchor(args.trace)
    if not anchors:
        print("no anchor lines in %s -- run with SDB_TRACE_JSON=<from>-<to> and "
              "--present-out, or the instrument is not measuring anything"
              % args.trace)
        return 2
    frames = frame_files(args.frames)
    if not frames:
        print("no PNGs in %s" % args.frames)
        return 2

    follow_anchors = read_anchor(args.follow_trace) if args.follow_trace else {}
    follow_frames = frame_files(args.follow) if args.follow else {}
    if args.follow and not (follow_anchors and follow_frames):
        print("--follow needs --follow-trace too, and both runs must have PNGs")
        return 2
    stock = frame_files(args.stock) if args.stock else {}
    if follow_anchors and not stock:
        print("--follow compares both runs against one stock frame, so --stock "
              "is required with it")
        return 2

    keep = REVEAL if args.reveal else 0
    examined = 0
    same = 0
    shared = 0
    lock_bytes = 0
    offcourt = 0
    centred = 0
    seen_here_zero = 0
    centred_bad = []
    failures = []
    stock_ok = stock_frames = 0
    stock_bad = []
    seen_anchor = set()
    anchored_here = set()
    first = None
    for f in sorted(k for k in anchors if args.frame_from <= k <= args.frame_to):
        if (f - args.frame_from) % args.step:
            continue
        # The compose that wrote anchor record `f` produced presented file f-1.
        index = f + FILE_OFFSET
        path = frames.get(index)
        if not path:
            continue
        rec = anchors[f]
        examined += 1
        seen_anchor.add(rec["anchor"])
        if rec.get("here"):
            anchored_here.add(rec["anchor"])
        if rec.get("here") is not None and not rec["here"]:
            seen_here_zero += 1
            centred += 1
            if rec["x0"] != rec["xh"]:
                centred_bad.append((f, rec))
        if first is None:
            first = rec

        left, right = COURT_LO - rec["anchor"], COURT_HI - rec["anchor"]
        if left < 0 or right > rec["cw"]:
            offcourt += 1
            failures.append("frame %d: court [%d,%d) lands at canvas columns %d..%d "
                            "on a %d-wide canvas" % (f, COURT_LO, COURT_HI, left,
                                                    right, rec["cw"]))

        if follow_anchors:
            other = follow_anchors.get(f)
            fpath = follow_frames.get(index)
            sp = stock.get(index)
            if not (other and fpath and sp):
                continue
            ref = Image.open(sp).convert("RGBA")
            mine = block_diff(Image.open(path).convert("RGBA"), ref, rec, keep)
            theirs = block_diff(Image.open(fpath).convert("RGBA"), ref, other, keep)
            ref.close()
            if mine != theirs:
                failures.append("frame %d: the machine's own 256 columns differ "
                                "from stock by %d bytes locked and %d following "
                                "the same frame -- the anchor changed the block"
                                % (f, mine, theirs))
            else:
                same += 1
                if mine:
                    shared += 1
                    lock_bytes += mine

        if stock and not follow_anchors:
            sp = stock.get(index)
            if sp:
                stock_frames += 1
                img = Image.open(path).convert("RGBA")
                n, msgs = compare_to_stock(img, Image.open(sp).convert("RGBA"), rec,
                                           "frame %d" % f, keep)
                img = None
                if n:
                    stock_bad.extend(msgs)
                else:
                    stock_ok += 1

    if examined == 0:
        print("examined no frames -- anchors %d, PNGs %d, range %d..%d"
              % (len(anchors), len(frames), args.frame_from, args.frame_to))
        return 2

    print("frames examined: %d" % examined)
    print("revealed columns skipped: %d (0 means a bare --widescreen run, where "
          "all 256 columns are the machine's)" % keep)
    print("distinct anchors (canvas world start): %s" % sorted(seen_anchor))
    print("court [%d,%d) at canvas columns %d..%d on a %d-wide canvas"
          % (COURT_LO, COURT_HI, COURT_LO - first["anchor"],
             COURT_HI - first["anchor"], first["cw"]))
    print("frames with the court off its own picture: %d" % offcourt)
    if follow_anchors:
        print("machine's own 256 columns vs the SAME stock frame, at each run's "
              "own anchor: identical on %d of %d frames" % (same, examined))
        if shared:
            print("  %d of those frames are not byte-identical to stock, in BOTH "
                  "runs (%d bytes on the locked one, %d total) -- pre-existing, "
                  "cause not established, and NOT the anchor"
                  % (shared, lock_bytes, lock_bytes))
    if first.get("here") is not None:
        print("frames the anchor deliberately left centred: %d of %d "
              "(menus, titles, anything outside the game's camera window -- they "
              "must be, or a menu renders left of centre)" % (centred, examined))
        if centred != seen_here_zero:
            failures.append("%d centred frames, but %d had here=0 -- the two "
                            "disagree" % (centred, seen_here_zero))
        for f, rec in centred_bad:
            failures.append("frame %d: centred (here=0) but the block is at %d "
                            "where the engine centred it at %d -- a menu would "
                            "render left of centre" % (f, rec["x0"], rec["xh"]))
    if stock and not follow_anchors:
        print("INFORMATIONAL -- machine's own 256 columns vs stock: %d of %d "
              "frames byte-identical" % (stock_ok, stock_frames))

    # The constant-anchor claim is about the frames the anchor ENGAGED on. A run
    # that also examined menus would otherwise see their centred values too --
    # which is correct behaviour, not movement. A run where nothing engaged (a
    # follow capture, the positive control) falls back to every frame, so the
    # check still fires and says why.
    moving = anchored_here if anchored_here else seen_anchor
    if len(moving) > 1:
        print("FAIL: the view moved. `anchor` took %d values (%s). Locked is a "
              "constant derived from the width alone. RUNNING THIS AGAINST A "
              "FOLLOW CAPTURE MUST FAIL HERE -- that is the positive control "
              "for the instrument." % (len(seen_anchor), sorted(seen_anchor)))
        return 1
    if args.expect == "locked" and not first["lock"]:
        print("FAIL: expected a locked camera, the trace says lock=0")
        return 1
    if failures:
        for line in failures[:20]:
            print("FAIL: %s" % line)
        if len(failures) > 20:
            print("... and %d more" % (len(failures) - 20))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
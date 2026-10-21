#!/usr/bin/env python3
"""basis_check.py - is the margin drawing its sprites where the GAME put them?

The margin draws the sprites the game culled. The game computes each sprite's
screen column in its 16-bit accumulator (`$04/$05`), and turns it into an OAM X
by subtracting 4 (`$84A1: SEC / SBC #$04`). That accumulator is the sprite's
PLACEMENT: the machine writes it to OAM and the PPU draws it at that column.

The margin used to place its parts somewhere else: it reconstructed the column
from the display RECORD (`record_X + part_offset - scroll - 4`), which is the
part's LIVE world position. The machine's copy of the same sprite is frozen at
the emission that produced it -- the game rewrites the record every frame and
emits an object's sprites every four -- so the two sat on different time bases
the moment the object moved, and a character straddling the machine's
256-column edge came apart.

    python3 tools/widescreen/basis_check.py TRACE.log

TWO POPULATIONS, and only one of them is a question.

The game culls a sprite when its accumulator is outside [4,255]. A character
that straddles the machine's 256-column edge has some of its sprites culled
(the margin's to draw) and some emitted (the machine's to draw) -- that is a
SHARED object, and there the margin's copy has to MEET the machine's, so every
drawn part must be at the accumulator the game computed for it. A character the
game culled WHOLE is the margin's alone, and it belongs at the object's live
world position instead: the accumulator is camera-relative and frozen at the
emission, so a character the game stopped emitting drifts away from where it
is as the camera moves.

The tool separates the two by the emission itself -- a $4D the game culled is
the margin's, a $4D it emitted is the machine's -- and reports each.

A REPORT, not a gate. The delta between where the margin drew a part and where
the game put it is not a defect on its own: a delta of one camera step (6-7
columns) is the camera having moved between the emission and the frame, which
the margin's own scroll-relative placement carries and the game's accumulator
does not. What a defect looks like is a delta that TRACKS THE OBJECT'S OWN
MOVEMENT -- the margin's copy running ahead of the machine's copy of the same
sprite -- and the tear it causes is a character split in two down the middle.

Exit 0 when it measured something, 2 when it measured nothing: an instrument
that measured nothing is not a pass.
"""

import re
import sys
import collections

Y0 = 236            # the game's Y accumulator to a screen row: first line = $EC - Y16


def s16(v):
    """The game's own 16-bit signed arithmetic. The accumulator is computed with
    SEC/SBC, so a sprite off the LEFT edge is a large unsigned value whose high
    byte is set -- which is exactly what the game's cull rejects."""
    v &= 0xFFFF
    return v - 65536 if v >= 32768 else v


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    # The accumulators the game last used for each object, by $4D.
    acc = collections.defaultdict(dict)
    per_frame = collections.defaultdict(list)

    for line in open(sys.argv[1], errors="replace"):
        m = re.match(r'^sdbp f=(\d+) obj=(\d+) at=\$([0-9A-F]+) \$02=(\d+) '
                     r'\$4D=(-?\d+) x=(-?\d+) y=(-?\d+)', line)
        if m:
            obj = int(m.group(2))
            if m.group(3) == '8483':
                d = int(m.group(5))
                if 0 <= d <= 7:
                    acc[obj][d] = (int(m.group(6)), int(m.group(7)))
            continue
        m = re.match(r'^sdbw f=(\d+) x0=(\d+):', line)
        if not m:
            continue
        f = int(m.group(1))
        per_obj = collections.defaultdict(list)
        order = []
        for tok in re.finditer(
                r'\[o(\d+)(?:n=\d+ id=[0-9A-F]{2}/[0-9A-F]{2} rec=[0-9A-F/]+'
                r'|:(-?\d+),(-?\d+),t([0-9A-F]{2}))\]', line):
            obj = int(tok.group(1))
            if obj not in per_obj:
                order.append(obj)
            if tok.group(2) is not None:
                per_obj[obj].append((int(tok.group(2)), int(tok.group(3))))
        for obj in order:
            # The accumulators AS OF THIS FRAME: the object is emitted every
            # four frames, and what matters is the emission this part belongs
            # to, not the last one in the run.
            per_frame[(f, obj)] = (per_obj[obj], dict(acc.get(obj, {})))

    shared_total = shared_ok = 0
    alone_total = 0
    bad = []
    for (f, obj), (parts, a) in per_frame.items():
        if not a:
            continue
        # The $4Ds the game culled are the margin's; the ones it emitted are the
        # machine's. A character with both is SHARED: one sprite, two renderers.
        culled = [(d, v) for d, v in sorted(a.items()) if v[0] < 4 or v[0] > 255]
        emitted = [d for d, v in a.items() if 4 <= v[0] <= 255]
        if not culled:
            continue
        shared = bool(emitted)
        # The margin's part list is in $4D order (the order the culled ones were
        # captured), and the drawn parts are a subsequence of it. Walk both in
        # order and check the rows agree, so a part is matched to the $4D that
        # produced it and not to a neighbour's.
        ci = 0
        for (sx, row) in parts:
            hit = None
            for k in range(ci, len(culled)):
                d, (x, y) = culled[k]
                if Y0 - y == row:
                    hit = (k, d, x)
                    break
            if not hit:
                continue
            k, d, x = hit
            ci = k + 1
            if shared:
                shared_total += 1
                if s16(sx) == s16(x - 4):
                    shared_ok += 1
                else:
                    bad.append((f, obj, d, s16(sx), s16(x - 4),
                                s16(sx) - s16(x - 4)))
            else:
                alone_total += 1

    if shared_total == 0 and alone_total == 0:
        print("basis_check: compared nothing -- the trace has no sdbw part lines "
              "that could be matched to an emission.")
        return 2

    print("SHARED objects (the machine drew part of the character too):")
    print("  parts compared: %d" % shared_total)
    print("  on the game's own basis: %d (%.1f%%)"
          % (shared_ok, 100.0 * shared_ok / max(shared_total, 1)))
    if bad:
        print("  OFF it: %d (%.1f%%)" % (len(bad), 100.0 * len(bad) / shared_total))
        print("  delta histogram: %s"
              % dict(sorted(collections.Counter(b[5] for b in bad).items(),
                            key=lambda kv: -kv[1])[:10]))
        frames = sorted({b[0] for b in bad})
        print("  frames affected: %d (first %s)" % (len(frames), frames[:12]))
        print("  first 10:")
        for b in bad[:10]:
            print("    f=%d obj=%d $4D=%d drawn at %d, the game put it at %d "
                  "(delta %+d)" % b)
    print()
    print("MARGIN-ONLY objects (the machine drew none of the character):")
    print("  parts: %d -- placed at the object's live world position, which is "
          "NOT the game's accumulator and is not checked here." % alone_total)
    return 0


if __name__ == '__main__':
    sys.exit(main())

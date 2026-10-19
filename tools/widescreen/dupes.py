#!/usr/bin/env python3
"""Duplicated margin parts, in the two shapes a duplicate actually takes.

**Same row, closer than 8.** Two parts of one object sharing a row and
overlapping horizontally.

`margin_track.py` reports a `ghost` when two parts of one object sit "within a
pixel of each other on a row". That is too narrow: the game's per-part offset
moves 2 to 7 columns between builds, so a stale copy lands 2-7 px from the part
that replaced it and the audit calls the frame clean. It did -- on a build that
visibly doubled sprites in play. This tool is the wide version of that check.

**Same column, different rows.** The other shape, and the one this tool's first
version was blind to, which is how a whole doubling slipped past it. Part
identity is (column, row), so when a RUNNING player's animation moves the ROW
between builds the replacement cannot match the part it replaces: the new part
appends and the old one is retained, until the object hits SDB_MARGIN_PARTS and
is pinned there. Measured on a played capture: object 11 (player 1) went 8 parts
-> 17 -> 24 while the committed build held 8 throughout, backstop 3, and the
picture showed a ghost silhouette with a hole in the torso. Same-column parts 1
to 7 rows apart are that artefact.

Both checks are deliberately narrow. Legitimate parts of one object do sit close
together -- a character's rows can be 1 px apart with columns 1 px apart -- so
"same column, 1 to 7 rows apart" is used rather than any near-match, because it
is the shape the artefact actually has rather than a guess at a tolerance.

  python3 tools/widescreen/dupes.py TRACE [from] [to]

The 8 is not a guess. Measured over the 1232-frame played capture in
a played session, every same-row pair the game emits is exactly 8 apart -- the
histogram has a single bucket -- because a character is sixteen columns of two
eight-wide sprites. Anything closer than 8 on one row is therefore two parts
where there should be one.

It reads the `sdbw` lines a `SDB_TRACE_CAPTURES=<from>-<to>` run writes, so it
needs that range set; without it the log has no `sdbw` lines and this reports
nothing, which is a broken instrument and not a clean picture.

**This is a comparative gate, not a zero gate.** Two parts of one object can
legitimately sit closer than 8 -- the committed build has 48 such pairs, all of
them in one 2-frame window at f=561 -- so demanding zero would fail a correct
build. What separates a real duplicate from a legal close pair is whether the gap
HOLDS as the object moves: two parts of one rigid object keep a constant gap and
drift together, while a stale copy's gap CHANGES every camera step, because the
copy does not move with the part that replaced it. So each cluster is classified
`rigid` or `drifting`, and **drifting clusters are the failures.** A drifting
cluster that persists to the last frame of a long capture is a sprite painted
permanently in two places.
"""
import argparse
import re
import sys
from collections import defaultdict

LINE = re.compile(r'^sdbw f=(\d+) x0=(\d+):(.*)$')
PART = re.compile(r'\[o(\d+):(-?\d+),(\d+),t([0-9A-F]{2})\]')
MIN_SPACING = 8


def collect(path, lo, hi):
    rows = []
    stacked = []
    frames = 0
    for raw in open(path):
        m = LINE.match(raw.strip())
        if not m:
            continue
        frames += 1
        f = int(m.group(1))
        if f < lo or f > hi:
            continue
        parts = defaultdict(list)
        for pm in PART.finditer(m.group(3)):
            parts[int(pm.group(1))].append((int(pm.group(2)), int(pm.group(3)), pm.group(4)))
        for obj, ps in parts.items():
            byrow = defaultdict(list)
            for dx, row, tile in ps:
                byrow[row].append((dx, tile))
            for row, xs in byrow.items():
                xs.sort()
                for i in range(1, len(xs)):
                    gap = xs[i][0] - xs[i - 1][0]
                    if gap < MIN_SPACING:
                        rows.append((f, obj, row, xs[i - 1], xs[i], gap))
            # Shape two: the SAME column on two rows close together.
            bycol = defaultdict(list)
            for dx, row, tile in ps:
                bycol[dx].append((row, tile))
            for col, rs in bycol.items():
                rs.sort()
                for i in range(1, len(rs)):
                    d = rs[i][0] - rs[i - 1][0]
                    if 0 < d < MIN_SPACING:
                        stacked.append((f, obj, col, rs[i - 1], rs[i], d))
    return frames, rows, stacked


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('lo', nargs='?', type=int, default=0)
    ap.add_argument('hi', nargs='?', type=int, default=1 << 30)
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--max-stacked', type=int, default=500,
                    help='fail above this many stacked pairs (default 500, which '
                         'sits above BOTH committed baselines -- 37 follow, 367 '
                         'locked -- and an order of magnitude below a build that '
                         'doubles: 1668 and 3114)')
    a = ap.parse_args()

    frames, rows, stacked = collect(a.trace, a.lo, a.hi)
    if not frames:
        print(f"{a.trace}: no sdbw lines. SDB_TRACE_CAPTURES was probably not set,\n"
              f"so this instrument measured nothing -- not a clean result.",
              file=sys.stderr)
        return 2

    # Classify by (object, row, tile pair): a rigid cluster holds one gap as the
    # object moves, a drifting one does not.
    clusters = defaultdict(list)
    for f, obj, row, p, q, gap in rows:
        key = (obj, row, p[1], q[1])
        clusters[key].append((f, p[0], q[0], gap))

    rigid, drifting = [], []
    for key, evs in clusters.items():
        (gaps := {e[3] for e in evs})
        (drifting if len(gaps) > 1 else rigid).append((key, evs))

    print(f"{a.trace}: {frames} sdbw frames, {len(rows)} pairs closer than "
          f"{MIN_SPACING}, over {len(clusters)} clusters")
    print(f"  {len(drifting)} drifting (FAILURES), {len(rigid)} rigid (legal)")
    byobj = defaultdict(int)
    for key, _ in drifting:
        byobj[key[0]] += 1
    if byobj:
        print("  drifting by object:", dict(sorted(byobj.items(), key=lambda kv: -kv[1])))

    # The stacked shape offers no rigid/drifting split: a retained part on an old
    # row and a fresh one on the new row are the same rigid body at two moments.
    # So it reports as a count and every entry is a failure.
    st_by_obj = defaultdict(int)
    for f, obj, col, p, q, d in stacked:
        st_by_obj[obj] += 1
    print(f"  {len(stacked)} stacked (same column, 1-{MIN_SPACING - 1} rows apart)")
    if st_by_obj:
        print("  stacked by object:", dict(sorted(st_by_obj.items(), key=lambda kv: -kv[1])))
    if a.verbose:
        for key, evs in sorted(drifting, key=lambda kv: kv[0][0]):
            obj, row, ta, tb = key
            gaps = sorted({e[3] for e in evs})
            span = f"{evs[0][0]}-{evs[-1][0]}"
            print(f"  drifting obj {obj} row {row} tiles {ta}/{tb}: {len(evs)} pairs over "
                  f"frames {span}, gaps {gaps}")
        for f, obj, col, p, q, d in stacked[:40]:
            print(f"  stacked f={f} obj {obj} col {col}: row {p[0]}/{q[0]} tiles "
                  f"{p[1]}/{q[1]} {d} rows apart")

    if drifting:
        print("FAIL: drifting clusters -- unambiguously wrong")
        return 1
    if len(stacked) > a.max_stacked:
        print(f"FAIL: {len(stacked)} stacked pairs, over --max-stacked "
              f"{a.max_stacked}")
        return 1
    print(f"PASS: {len(stacked)} stacked, under --max-stacked {a.max_stacked}")
    return 0


if __name__ == '__main__':
    sys.exit(main())

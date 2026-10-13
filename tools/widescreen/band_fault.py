#!/usr/bin/env python3
"""band_fault.py - find a fault that is confined to the 8 overscan-revealed columns.

The band is the only part of the canvas the machine does not own. With
`--overscan` on, native columns 0-7 are not copied back from the machine; they
keep what the compositor reconstructed from the nametable, plus any margin
sprite drawn there. So anything wrong in that strip is either a reconstruction
fault or a margin-sprite fault, and the two are separable: run the same schedule
with `--margin-sprites 0` and diff. What remains is reconstruction.

The check is the obvious one and it is worth stating why it is not trivially
true. The court is a 512-pixel looping stadium, so a bar drawn across the world
is at the same ROW in the band as it is eight columns to the right -- if the
reconstruction is right. A sliver of the bar surviving in the band while the
machine's own columns beside it have moved on is exactly what a wrong scroll, a
stale capture, or a sprite painted into a column the machine will overwrite all
look like. So: for every canvas column, find the first and last row matching the
bar's colour, and compare across the band edge.

    ./build-cycle/nes_game ROM --frames 700 --input tas/vectors/sdb_bar.input \\
        --ram-init zeros --widescreen 16:9 --overscan \\
        --present-out tmp/bf/on_ --present-every 1
    ./build-cycle/nes_game ROM --frames 700 --input tas/vectors/sdb_bar.input \\
        --ram-init zeros --widescreen 16:9 --overscan --margin-sprites 0 \\
        --present-out tmp/bf/off_ --present-every 1
    python3 tools/widescreen/band_fault.py tmp/bf --frames 500:700

Reports, per frame: any column whose bar rows disagree with the machine's own
columns, separated into reconstruction (present with margin sprites off) and
margin-sprite (only present with them on).
"""
import argparse
import glob
import os
import re
import sys

import numpy as np
from PIL import Image

# The court's dark red bar, sampled from the presented picture rather than
# guessed: (120, 10, 0).
BAR = np.array([120, 10, 0])
BAND_LO, BAND_HI = 85, 93          # canvas columns, at 16:9 with overscan
NATIVE_W = 256
TOL = 40                           # sum of per-channel error still counted as the bar


def frames(pattern):
    out = []
    for p in glob.glob(pattern):
        m = re.search(r'(\d+)\.png$', p)
        if m:
            out.append((int(m.group(1)), p))
    return dict(sorted(out))


def bar_rows(img, x, y0=90, y1=239):
    """(first, last) row of the bar in one canvas column, or None."""
    col = img[y0:y1, x]
    hit = np.where(np.abs(col - BAR).sum(axis=1) < TOL)[0]
    return (int(hit[0]) + y0, int(hit[-1]) + y0) if len(hit) else None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="directory holding the on_/off_ present-out runs")
    ap.add_argument("--frames", metavar="LO:HI", default="0:99999")
    ap.add_argument("--band", metavar="LO:HI", default=f"{BAND_LO}:{BAND_HI}")
    args = ap.parse_args()

    on = frames(os.path.join(args.dir, "on__*.png"))
    off = frames(os.path.join(args.dir, "off__*.png"))
    if not on or not off:
        raise SystemExit(f"{args.dir}: need both on__*.png and off__*.png")
    lo, hi = (int(v) for v in args.frames.split(":"))
    blo, bhi = (int(v) for v in args.band.split(":"))

    width = np.array(Image.open(next(iter(on.values())))).shape[1]
    x0 = (width - NATIVE_W) // 2
    if (x0, x0 + NATIVE_W) != (BAND_LO, BAND_HI):
        print(f"note: native_x0 is {x0}, so the revealed band is {x0}..{x0 + 7} "
              f"and the machine's picture starts at {x0 + 8}")
        blo, bhi = x0, x0 + 8

    faults = {"reconstruction": [], "margin sprite": []}
    common = sorted(set(on) & set(off))
    for n in common:
        if not (lo <= n <= hi):
            continue
        for kind, table in (("reconstruction", off), ("margin sprite", on)):
            img = np.array(Image.open(table[n]).convert("RGB")).astype(int)
            # the machine's own column, immediately right of the band
            ref = bar_rows(img, bhi)
            if ref is None:
                continue
            for x in range(blo, bhi):
                got = bar_rows(img, x)
                if got and got != ref:
                    faults[kind].append((n, x, got, ref))

    total = sum(len(v) for v in faults.values())
    if not total:
        print(f"{len(common)} frames checked, columns {blo}..{bhi - 1}: "
              f"the bar is continuous across the band edge on every frame")
        return 0
    for kind, rows in faults.items():
        if not rows:
            continue
        frames_hit = sorted({n for n, _x, _g, _r in rows})
        print(f"{kind}: {len(rows)} column-frames across {len(frames_hit)} frames "
              f"(first {frames_hit[:8]})")
        for n, x, got, ref in rows[:24]:
            print(f"   frame {n:5d} col {x:3d}: bar rows {got}, machine's {ref}")
        if len(rows) > 24:
            print(f"   ... {len(rows) - 24} more")
    return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""char_split.py — group OAM slots into characters and report completeness.

A character is a run of consecutive slots forming 2-tile rows 8px apart.  A run
with a row holding only one tile (or fewer rows than its neighbours) was cut by
the 64-sprite budget.  Reports each cut character by its X so we can see whether
the SAME character flickers in and out, or different ones get cut.
"""
import struct
import sys
from collections import defaultdict

def parse(path):
    d = open(path, "rb").read()
    off = 12
    out = []
    while off + 36 <= len(d):
        h = struct.unpack_from("<9I", d, off)
        fr = h[0]
        l_ciram, l_cart, l_chr, l_fds, l_pic = h[4], h[5], h[6], h[8], h[7]
        off += 36
        oam_at = off + 32
        off += 32 + 256 + 0x800 + l_ciram + l_cart + l_chr + l_fds + l_pic
        out.append((fr, d[oam_at:oam_at + 256]))
    return out

def group(oam):
    sp = [(j, oam[j*4]+1, oam[j*4+1], oam[j*4+2], oam[j*4+3])
          for j in range(64) if oam[j*4] < 0xEF]
    chars = []
    cur = []
    prev_y = None
    for slot, y, t, a, x in sp:
        if prev_y is not None and y < prev_y:
            chars.append(cur); cur = []
        cur.append((slot, y, t, x)); prev_y = y
    if cur: chars.append(cur)
    # a character is complete when every 2-tile row has both halves
    out = []
    for c in chars:
        rows = defaultdict(list)
        for slot, y, t, x in c:
            rows[y].append(x)
        full_rows = sum(1 for y, xs in rows.items() if len(xs) == 2)
        partial = [y for y, xs in rows.items() if len(xs) == 1]
        if partial and len(c) >= 2:
            out.append((c[0][0], c[0][3], len(c), sorted(rows), partial))
    return out, sp

def main(path, lo, hi):
    rows = [r for r in parse(path) if lo <= r[0] <= hi]
    print(f"frames {lo}..{hi}")
    seen = defaultdict(lambda: [0, 0])   # X -> [frames_cut, frames_seen]
    for fr, oam in rows:
        bad, sp = group(oam)
        for slot0, x0, n, ys, partial in bad:
            seen[x0][0] += 1
        for c in [x for x in seen if x]:
            pass
        if bad and fr <= lo + 8:
            for slot0, x0, n, ys, partial in bad:
                print(f"  frame {fr}: CUT character at slot {slot0}, X={x0}, "
                      f"{n} tiles, rows {ys}, partial rows {partial}")
    # per-X frequency
    print("\nX position -> frames it was cut / frames observed as a partial row")
    for x in sorted(seen):
        print(f"  X={x:3d}: cut {seen[x][0]} frames")
    print("\nframes analysed:", len(rows))

if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
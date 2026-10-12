#!/usr/bin/env python3
"""truncation.py — which character loses tiles, and does it move?

A complete character in this game is a 2-tile-wide column: two sprites sharing
a screen Y, X differing by 8.  A lone sprite at a Y where its partner is absent
is a truncated character.
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
        l_ciram, l_cart, l_chr, l_fds = h[4], h[5], h[6], h[8]
        l_chr, l_pic = h[6], h[7]
        off += 36
        oam_at = off + 32
        off += 32 + 256 + 0x800 + l_ciram + l_cart + l_chr + l_fds + l_pic
        out.append((fr, d[oam_at:oam_at + 256]))
    return out

def orphans(oam):
    sp = [(j, oam[j*4]+1, oam[j*4+1], oam[j*4+2], oam[j*4+3])
          for j in range(64) if oam[j*4] < 0xEF]
    by_y = defaultdict(list)
    for slot, y, t, a, x in sp:
        by_y[y].append((slot, x, t))
    bad = []
    for y, items in sorted(by_y.items()):
        if len(items) == 1:
            slot, x, t = items[0]
            bad.append((slot, y, x, t))
    return bad, sp

def main(path, lo, hi):
    rows = [r for r in parse(path) if lo <= r[0] <= hi]
    print(f"frames {lo}..{hi}  ({len(rows)} frames)")
    tot = 0
    for fr, oam in rows:
        bad, sp = orphans(oam)
        if bad:
            tot += 1
            where = ", ".join(f"slot{b[0]}@Y{b[1]},X{b[2]}" for b in bad[:4])
            print(f"  frame {fr}: {len(sp):2d} sprites, {len(bad)} truncated tile(s): {where}")
    print(f"\n{tot}/{len(rows)} frames have a truncated character")

if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
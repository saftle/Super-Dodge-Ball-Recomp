#!/usr/bin/env python3
"""drop_report.py — per-frame proof of the 8-sprite-per-line hardware drop.

For each OAM sprite in 8x16 mode, decide whether the real PPU would have
rendered it: the hardware evaluates OAM in slot order and keeps the first
eight sprites overlapping a scanline; anything later is invisible.

Prints, per frame, the sprites that are IN OAM but NOT renderable, which is
exactly the "lower half of the body is not shown" symptom.
"""
import struct
import sys
from collections import defaultdict

def parse(path, want):
    d = open(path, "rb").read()
    off = 12
    out = []
    while off + 36 <= len(d):
        h = struct.unpack_from("<9I", d, off)
        fr = h[0]
        l_ciram, l_cart, l_chr, l_pic, l_fds = h[4], h[5], h[6], h[7], h[8]
        base = off
        off += 36
        oam_at = off + 32
        off += 32 + 256 + 0x800 + l_ciram + l_cart + l_chr + l_fds + l_pic
        if fr in want:
            out.append((fr, d[oam_at:oam_at + 256]))
    return out

def main(path, frames, tall=True):
    rows = tall and 16 or 8
    for fr, oam in parse(path, set(frames)):
        sp = [(j, oam[j*4], oam[j*4+1], oam[j*4+2], oam[j*4+3])
              for j in range(64) if oam[j*4] < 0xEF]
        used = defaultdict(int)
        dropped = []
        for slot, y, t, a, x in sp:                       # slot order
            hit_any = False
            for r in range(rows):
                ly = y + 1 + r
                if not (0 <= ly < 240):
                    continue
                if used[ly] >= 8:
                    hit_any = True
                else:
                    used[ly] += 1
            if hit_any:
                dropped.append((slot, y, t, a, x))
        over = {k: v for k, v in used.items() if v >= 8}
        print(f"frame {fr}: {len(sp)} sprites in OAM, {len(dropped)} INVISIBLE on real HW "
              f"({len(over)} scanlines at the limit)")
        for slot, y, t, a, x in dropped:
            print(f"    dropped slot {slot:2d}: Y={y:3d} (screen y {y+1:3d}) "
                  f"tile=${t:02X} attr=${a:02X} X={x:3d}")

if __name__ == "__main__":
    fs = [int(v) for v in sys.argv[2:]] or [450]
    main(sys.argv[1], fs, tall=True)
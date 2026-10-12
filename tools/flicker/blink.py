#!/usr/bin/env python3
"""blink.py — Enemy B's 5-on/4-off blink, measured off the CYCFRAME log.

Per frame: is B's sprite cluster in OAM? What is the OAM slot range, sprite
count, and the RAM the continuation prompt flagged as suspects?
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
        pal_at = off
        oam_at = off + 32
        ram_at = oam_at + 256
        off += 32 + 256 + 0x800 + l_ciram + l_cart + l_chr + l_fds + l_pic
        out.append((fr, d[pal_at:pal_at + 32], d[oam_at:oam_at + 256],
                    d[ram_at:ram_at + 0x800]))
    return out


def sprites(oam):
    return [(j, oam[j * 4], oam[j * 4 + 1], oam[j * 4 + 2], oam[j * 4 + 3])
            for j in range(64) if oam[j * 4] < 0xEF]


def find_b(sp):
    """B: tiles $8D/$8E, attr low nibble 1, screen Y ~168, X ~163 (sprite x)."""
    hits = [(s, y, t, a, x) for s, y, t, a, x in sp
            if t in (0x8D, 0x8E) and (a & 3) == 1 and 160 <= y + 1 <= 180]
    return hits


def main(path, lo, hi):
    rows = [r for r in parse(path) if lo <= r[0] <= hi]
    print(f"frames {lo}..{hi} ({len(rows)})")
    print("fr  nsp  B(slots/y/x)                          $03F5 $062D $067A $0617 tail")
    prev = None
    for fr, pal, oam, ram in rows:
        sp = sprites(oam)
        b = find_b(sp)
        cur = ram[0x0617]
        tag = ""
        if b:
            slots = ",".join(str(s[0]) for s in b)
            ys = sorted(set(s[1] for s in b))
            xs = sorted(set(s[4] for s in b))
            tag = f"ON  [{slots}] y={ys[0]}..{ys[-1]} x={xs}"
        else:
            tag = "off"
        print(f"{fr:5d} {len(sp):4d}  {tag:36s} "
              f"{ram[0x03F5]:02X}   {ram[0x062D]:02X}   {ram[0x067A]:02X}   "
              f"{cur:02X}   $067C={[ram[0x067C+i] | (ram[0x067D+i]<<8) for i in range(0,16,2)]}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
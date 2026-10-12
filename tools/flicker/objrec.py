#!/usr/bin/env python3
"""objrec.py — dump the big object state record (the $C988 table) per frame.

$C988 maps object id -> $4D/$4E pointer from the table at $C998
($031F, $0357, $038F, $03C7, $03FF, $0437, $046F, $04A7, $04DF, $0517, $054F,
$0587, $0585) -- stride $38.  The display record at $0618+7*i is derived from
these; record[3].byte0 bit 5 is the game's own suppress flag.
"""
import struct
import sys

BIGN = 13
STRIDE = 0x38
BASE = 0x031F


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
        ram_at = oam_at + 256
        off += 32 + 256 + 0x800 + l_ciram + l_cart + l_chr + l_fds + l_pic
        out.append((fr, d[oam_at:oam_at + 256], d[ram_at:ram_at + 0x800]))
    return out


def main(path, lo, hi, obj, stride=7):
    rows = [r for r in parse(path) if lo <= r[0] <= hi]
    prev = None
    for fr, oam, ram in rows:
        b = BASE + obj * STRIDE
        rec = ram[b:b + STRIDE]
        cnt = sum(1 for j in range(64) if oam[j * 4] < 0xEF)
        diff = "" if prev is None else "".join(
            f" +{i}:{prev[i]:02X}>{rec[i]:02X}" for i in range(len(rec)) if prev[i] != rec[i])
        print(f"f{fr} nsp={cnt:2d} disp3=sup{'P' if ram[0x062D] & 0x20 else '.'} "
              f"obj{obj}@${b:04X}:" + " ".join(f"{v:02X}" for v in rec) + diff)
        prev = rec


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
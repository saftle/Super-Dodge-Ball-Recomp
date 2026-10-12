#!/usr/bin/env python3
"""records.py — the object record table at $0618 (7 bytes x 13 objects).

$821E loads record[0] into $4E, and $828A does `LDA $4E / AND #$20 / BNE RTS`:
bit 5 of record byte 0 is the game's own "do not draw this object" flag.
Prints every record per frame so we can see who flips it and when.
"""
import struct
import sys

BASE = 0x0618
N = 13
STRIDE = 7


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


def records(ram):
    return [tuple(ram[BASE + i * STRIDE:BASE + i * STRIDE + STRIDE])
            for i in range(N)]


def main(path, lo, hi, show=0):
    rows = [r for r in parse(path) if lo <= r[0] <= hi]
    prev = None
    for fr, oam, ram in rows:
        recs = records(ram)
        ids = [(ram[0x067C + i] | (ram[0x067D + i] << 8)) for i in range(ram[0x067A])]
        cnt = sum(1 for j in range(64) if oam[j * 4] < 0xEF)
        if show:
            print(f"frame {fr}  nsp={cnt:2d}  ids={[f'{v:04X}' for v in ids]}")
            for i, r in enumerate(recs):
                mark = ""
                if r[0] & 0x20:
                    mark += " [bit5=SUPPRESS]"
                if prev and prev[i] != r:
                    mark += f" <- was {prev[i].hex()}"
                print(f"   obj{i:2d} ${BASE+i*STRIDE:04X}: "
                      + " ".join(f"{b:02X}" for b in r) + mark)
            prev = recs
        else:
            flags = "".join("X" if r[0] & 0x20 else "." for r in recs)
            print(f"{fr:5d} nsp={cnt:2d} $067A={ram[0x067A]:02X} suppress=[{flags}] "
                  f"obj3={''.join(f'{b:02X}' for b in recs[3])} "
                  f"ids={[f'{v:04X}' for v in ids]}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]),
         int(sys.argv[4]) if len(sys.argv) > 4 else 0)
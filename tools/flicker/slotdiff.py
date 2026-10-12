#!/usr/bin/env python3
"""slotdiff.py — print OAM slot-by-slot for a frame range, marking changes."""
import struct
import sys


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


def dump(oam, mark=()):
    for j in range(64):
        y, t, a, x = oam[j * 4:j * 4 + 4]
        flag = " <<<" if j in mark else ""
        print(f"  slot{j:2d}: y={y:3d}(s{y+1:3d}) tile=${t:02X} attr=${a:02X} x={x:3d}{flag}"
              + ("   [hidden]" if y >= 0xEF else ""))


def main(path, frames):
    want = set(int(v) for v in frames)
    rows = {fr: (o, r) for fr, o, r in parse(path) if fr in want}
    for fr in sorted(rows):
        o, r = rows[fr]
        print(f"=== frame {fr}  cursor=${r[0x0617]:02X} $03F5=${r[0x03F5]:02X} "
              f"$062D=${r[0x062D]:02X} $067A=${r[0x067A]:02X} "
              f"cnt={sum(1 for j in range(64) if o[j*4] < 0xEF)}")
        dump(o)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
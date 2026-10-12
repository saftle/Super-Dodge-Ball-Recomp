#!/usr/bin/env python3
"""oam_analyze.py — what does Super Dodge Ball actually put in OAM?

Reports, per frame: active sprites, peak sprites-per-visible-scanline under
both the 8x8 and 8x16 assumptions, and how many frames exceed the real
hardware 8-sprites-per-line limit.
"""
import struct
import sys
from collections import Counter

def parse(path):
    data = open(path, "rb").read()
    if data[:8] != b"CYCFRAME":
        raise SystemExit("bad magic: not a CYCFRAME log")
    ver, = struct.unpack_from("<I", data, 8)
    off = 12
    frames = []
    while off + 36 <= len(data):
        hdr = struct.unpack_from("<9I", data, off)
        frame = hdr[0]
        l_ciram, l_cart, l_chr, l_pic, l_fds = hdr[4], hdr[5], hdr[6], hdr[7], hdr[8]
        off += 36
        pal = data[off:off + 32]; off += 32
        oam = data[off:off + 256]; off += 256
        ram = data[off:off + 0x800]; off += 0x800
        off += l_ciram + l_cart + l_chr + l_fds
        pic = data[off:off + l_pic]; off += l_pic
        frames.append((frame, pal, oam, ram, pic))
    return frames

def sprites(oam):
    out = []
    for i in range(64):
        y, t, a, x = oam[i*4:i*4+4]
        if y >= 0xEF:
            continue
        out.append((i, y, t, a, x))
    return out

def peak(sp, rows):
    lines = Counter()
    for _i, y, _t, _a, _x in sp:
        for r in range(rows):
            ly = y + 1 + r
            if 0 <= ly < 240:
                lines[ly] += 1
    return max(lines.values()) if lines else 0

def main(path):
    frames = parse(path)
    rows = []
    for frame, pal, oam, ram, pic in frames:
        sp = sprites(oam)
        p8 = peak(sp, 8)
        p16 = peak(sp, 16)
        rows.append((frame, len(sp), p8, p16))
    n = len(rows)
    over8 = [r for r in rows if r[2] > 8]
    over16 = [r for r in rows if r[3] > 8]
    print(f"{n} frames from {path}")
    print(f"active sprites/frame: avg {sum(r[1] for r in rows)/n:.1f}  max {max(r[1] for r in rows)}")
    print(f"peak/line assuming 8x8  : max {max(r[2] for r in rows)}   frames over 8: {len(over8)}")
    print(f"peak/line assuming 8x16 : max {max(r[3] for r in rows)}   frames over 8: {len(over16)}")
    print("\nframe sprites p8 p16")
    for r in rows[::max(1, n // 45)]:
        print(f"{r[0]:5d} {r[1]:7d} {r[2]:3d} {r[3]:4d}" + ("  OVER8" if r[3] > 8 else ""))

if __name__ == "__main__":
    main(sys.argv[1])
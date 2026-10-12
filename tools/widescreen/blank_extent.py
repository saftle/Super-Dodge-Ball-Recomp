#!/usr/bin/env python3
"""blank_extent.py - the largest border crop that removes nothing.

Backs the research.md section 25 overscan table: measured over every frame of
one or more --frame-log logs, which rows and columns carry a non-backdrop pixel,
and therefore how much may be cropped without losing anything.

    nes_game ROM --frames 1100 --input tas/vectors/sdb_throw.input \\
        --ram-init zeros --frame-log tmp/ws/throw.bin --frame-log-frames 0:1099
    python3 tools/widescreen/blank_extent.py tmp/ws/throw.bin tmp/ws/idle.bin

There is no "index 0 means background" rule on the NES -- the universal
backdrop is whatever the game picks ($0F here) -- so the backdrop is taken per
frame as the most common index and anything else counts as ink. Fully blank
frames are skipped: they prove nothing.

Read the result with the game in mind. For Super Dodge Ball the left 8 columns
carry ink in 0 of 2112 frames, which is what the widescreen mod's `overscan`
feature removes; the right 8 carry the court, and the blank top and bottom
bands are used by frame 420, a full-screen wipe -- so neither side nor the
vertical bands may be cut.
"""
import struct
import sys
from collections import Counter


def frames(path):
    with open(path, "rb") as f:
        if f.read(8) != b"CYCFRAME":
            raise SystemExit("not a CYCFRAME log")
        f.read(4)
        while True:
            head = f.read(36)
            if len(head) < 36:
                return
            (fr, _, _, l_ram, l_ciram, l_cart, l_chr, l_pic, l_aud) = struct.unpack("<9I", head)
            f.read(32)
            f.read(256)
            f.read(l_ram)
            f.read(l_ciram)
            f.read(l_cart)
            f.read(l_chr)
            pic = f.read(l_pic)
            f.read(l_aud)
            yield fr, pic


def main():
    paths = sys.argv[1:]
    if not paths:
        raise SystemExit(__doc__)
    rows = [0] * 240
    cols = [0] * 256
    total = blank = 0
    backdrops = Counter()
    odd = []
    for p in paths:
        for fr, pic in frames(p):
            vals = struct.unpack("<%dH" % (256 * 240), pic)
            bg = Counter(v & 0x3F for v in vals).most_common(1)[0][0]
            backdrops[bg] += 1
            rowink = [0] * 240
            colink = [0] * 256
            for y in range(240):
                base = y * 256
                n = 0
                for x in range(256):
                    if (vals[base + x] & 0x3F) != bg:
                        n += 1
                        colink[x] += 1
                rowink[y] = n
            if not any(rowink):
                blank += 1
                continue
            total += 1
            for y in range(240):
                if rowink[y]:
                    rows[y] += 1
            for x in range(256):
                if colink[x]:
                    cols[x] += 1
            # Remember the rare frames that put ink in an otherwise blank band.
            if rowink[0] or rowink[239]:
                odd.append((p, fr, rowink[0], rowink[239]))

    print(f"frames with ink: {total}   fully blank frames skipped: {blank}")
    print(f"backdrop colour indices seen: {dict(backdrops.most_common(3))}")

    def run_up(counts, limit):
        n = 0
        for i in range(limit):
            if counts[i]:
                break
            n += 1
        return n

    def run_down(counts, limit):
        n = 0
        for i in range(limit):
            if counts[len(counts) - 1 - i]:
                break
            n += 1
        return n

    print(f"\nTOP rows blank at the start:    {run_up(rows, 40)} of 40")
    print(f"BOTTOM rows blank at the end:   {run_down(rows, 40)} of 40")
    print(f"LEFT columns blank at the start: {run_up(cols, 24)} of 24")
    print(f"RIGHT columns blank at the end:  {run_down(cols, 24)} of 24")
    print("\nper-band frame counts (0 = blank in EVERY frame):")
    print("  rows :", " ".join(f"{y}:{rows[y]}" for y in range(0, 32)))
    print("  cols :", " ".join(f"{x}:{cols[x]}" for x in range(0, 16)),
          "...", " ".join(f"{x}:{cols[x]}" for x in range(240, 256)))
    if odd:
        print("\nframes with ink in the top or bottom row (these block a vertical crop):")
        for p, fr, top, bot in odd[:10]:
            print(f"  {p} frame {fr}: top row {top}/256 ink, bottom row {bot}/256")


if __name__ == "__main__":
    main()
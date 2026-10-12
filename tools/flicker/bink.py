#!/usr/bin/env python3
"""bink.py — per-frame "ink" in Enemy B's on-screen box, from --present-out PNGs.

Ink = pixels in the box that differ from the box's most common colour (the
court behind him). B's sprite is 16x32 at sprite y=167 (screen y 168), x
163..178, so the box is a little wider than that.
"""
import sys
from collections import Counter
from PIL import Image

BOX = (158, 164, 186, 198)


def ink(path):
    im = Image.open(path).convert("RGB").crop(BOX)
    px = list(im.getdata())
    bg = Counter(px).most_common(1)[0][0]
    return sum(1 for p in px if p != bg), bg


def main(pat, lo, hi, label):
    print(f"--- {label} ---")
    vals = []
    for f in range(lo, hi + 1):
        n, bg = ink(f"{pat}_%05d_{f:05d}.png")
        vals.append((f, n))
    for f, n in vals:
        print(f"  f{f}: ink={n}")
    ns = [n for _, n in vals]
    mean = sum(ns) / len(ns)
    var = sum((n - mean) ** 2 for n in ns) / len(ns)
    print(f"  frames={len(ns)} min={min(ns)} max={max(ns)} "
          f"stdev={var ** 0.5:.2f}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4])
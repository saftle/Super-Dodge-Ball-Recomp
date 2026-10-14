#!/usr/bin/env python3
"""nametable.py - the court's world, read out of CIRAM.

Backs the research.md section 25 claim that Super Dodge Ball's court is a
512-pixel-wide looping stadium that is static for the whole match, which is what
lets the widescreen compositor read margin pixels out of the nametable instead
of remembering them (the way wideNES does).

    nes_game ROM --frames 1000 --input tas/vectors/sdb_throw.input \\
        --ram-init zeros --frame-log tmp/ws/fl.bin --frame-log-frames 640:760
    python3 tools/widescreen/nametable.py tmp/ws/fl.bin --rows 14,16,18,24,28
    python3 tools/widescreen/nametable.py tmp/ws/fl.bin --stability

CIRAM is 2 KiB pre-mirroring. The cartridge is HORIZONTALLY mirroring, so
$2000-$23FF and $2400-$27FF are two distinct pages -- 32x30 tiles side by side,
512 pixels -- and $2800/$2C00 alias them rather than holding a separate
attribute table. Attributes therefore live at page offset 0x3C0 ($23C0/$27C0),
which is why an attribute address built the textbook way reads tile bytes.
"""
import sys

from cycframe import records as _records


def records(path):
    for r in _records(path, want=("frame", "ciram")):
        yield r.frame, r.ciram


def show(ciram, rows):
    for y in rows:
        row = list(ciram[y * 32:y * 32 + 32]) + list(ciram[0x400 + y * 32:0x400 + y * 32 + 32])
        print(f" y={y:2d} | " + " ".join(f"{t:02X}" for t in row[:32])
              + " | " + " ".join(f"{t:02X}" for t in row[32:]))
    for base, label in ((0x3C0, "NT0 attributes"), (0x7C0, "NT1 attributes")):
        print(f" {label}: " + " ".join(f"{b:02X}" for b in ciram[base:base + 16]))


def stability(path, lo, hi, rows):
    """Are these rows still identical frame to frame? Static content is what
    makes the margin reconstruction exact instead of a guess."""
    prev = None
    changed = 0
    n = 0
    for fr, ciram in records(path):
        if not (lo <= fr <= hi):
            prev = None
            continue
        sel = [ciram[y * 32:(y + 1) * 32] for y in rows]
        if prev is not None:
            changed += sum(1 for a, b in zip(sel, prev) for x, y in zip(a, b) if x != y)
            n += 1
        prev = sel
    print(f" rows {rows}: {changed} differing tile bytes over {n} frame-to-frame "
          f"comparisons across frames {lo}..{hi}")


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    path = args[0]
    if "--stability" in args:
        lo = int(args[args.index("--stability") + 1])
        hi = int(args[args.index("--stability") + 2])
        rows = [int(x) for x in args[args.index("--stability") + 3].split(",")]
        stability(path, lo, hi, rows)
        return
    rows = [int(x) for x in (args[args.index("--rows") + 1] if "--rows" in args
                             else "14,16,18,24,28").split(",")]
    for fr, ciram in records(path):
        print(f"=== frame {fr}, {len(ciram)} bytes of CIRAM ===")
        show(ciram, rows)
        return


if __name__ == "__main__":
    main()
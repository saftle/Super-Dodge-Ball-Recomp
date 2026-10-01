#!/usr/bin/env python3
"""bk2 -> canonical TAS JSON.

Usage:
  python3 tools/tas/bk2_to_tas.py tas/vectors/sdb_4976.bk2 tas/sdb_4976.tas.json [--frames N]

Canonical format: {"rom_md5_headerless":..., "frames":[mask,...]} with recomp
mask bits A=0x80 B=0x40 SELECT=0x20 START=0x10 UP=0x08 DOWN=0x04 LEFT=0x02 RIGHT=0x01.
bk2 P1 columns: Up,Down,Left,Right,Start,Select,B,A ('.' = off).
Frame 0 is the power-on marker line (kept as frame 0, input normally empty).
"""
import json
import sys
import zipfile

RECOMP_MASK = {
    "A": 0x80, "B": 0x40, "SELECT": 0x20, "START": 0x10,
    "UP": 0x08, "DOWN": 0x04, "LEFT": 0x02, "RIGHT": 0x01,
}
# P1 field char index -> button (Up,Down,Left,Right,Start,Select,B,A)
BK2_COLS = ["UP", "DOWN", "LEFT", "RIGHT", "START", "SELECT", "B", "A"]


def parse_bk2(path):
    z = zipfile.ZipFile(path)
    log = z.read("Input Log.txt").decode("utf-8", "replace").splitlines()
    frames = [ln for ln in log if ln.startswith("|")]
    masks = []
    for ln in frames:
        parts = ln.split("|")
        # ['', powerreset, p1, p2, ''] — P1 at index 2
        p1 = parts[2] if len(parts) > 2 else ""
        m = 0
        for i, btn in enumerate(BK2_COLS):
            if i < len(p1) and p1[i] not in (".", " "):
                m |= RECOMP_MASK[btn]
        masks.append(m)
    header = z.read("Header.txt").decode("utf-8", "replace")
    return masks, header


def main():
    src = sys.argv[1]
    dst = sys.argv[2]
    maxf = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else None
    # allow --frames N form
    if "--frames" in sys.argv:
        maxf = int(sys.argv[sys.argv.index("--frames") + 1])
    masks, header = parse_bk2(src)
    if maxf:
        masks = masks[:maxf]
    md5 = ""
    for line in header.splitlines():
        if line.upper().startswith("MD5"):
            md5 = line.split(None, 1)[1].strip() if len(line.split(None, 1)) > 1 else ""
    out = {"source": src, "rom_md5_headerless": md5,
           "mask_map": "A=0x80 B=0x40 SELECT=0x20 START=0x10 UP=0x08 DOWN=0x04 LEFT=0x02 RIGHT=0x01",
           "frames": masks}
    with open(dst, "w") as f:
        json.dump(out, f)
    # summary: input runs
    runs = 0
    for i, m in enumerate(masks):
        if m and (i == 0 or masks[i - 1] == 0):
            runs += 1
    print(f"wrote {dst}: {len(masks)} frames, {sum(1 for m in masks if m)} with input, {runs} runs, md5={md5}")


if __name__ == "__main__":
    main()

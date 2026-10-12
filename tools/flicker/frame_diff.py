#!/usr/bin/env python3
"""frame_diff.py — consecutive-frame picture diff from a CYCFRAME log.

Locates the flicker precisely: which pixels change between frame N and N+1,
clustered into bounding boxes, and matched against OAM so we can tell whether
a sprite that vanished was ever IN OAM (hardware-limit drop) or absent from
OAM (the game culled it before the PPU ever saw it).
"""
import struct
import sys

def parse(path, want=None):
    data = open(path, "rb").read()
    assert data[:8] == b"CYCFRAME"
    off = 12
    out = []
    while off + 36 <= len(data):
        hdr = struct.unpack_from("<9I", data, off)
        frame = hdr[0]
        l_ciram, l_cart, l_chr, l_pic, l_fds = hdr[4], hdr[5], hdr[6], hdr[7], hdr[8]
        off += 36
        pal = data[off:off+32]; off += 32
        oam = data[off:off+256]; off += 256
        off += 0x800 + l_ciram + l_cart + l_chr + l_fds
        pic = data[off:off+l_pic]; off += l_pic
        if want is None or frame in want:
            idx = list(struct.unpack(f"<{256*240}H", pic[:256*240*2]))
            out.append((frame, pal, oam, idx))
    return out

def changed_pixels(a, b):
    """Coordinates where the picture index differs."""
    pts = []
    for p in range(256*240):
        if a[p] != b[p]:
            pts.append((p % 256, p // 256, a[p], b[p]))
    return pts

def cluster(pts, gap=4):
    """Group changed pixels into rough boxes by connected components on a grid."""
    remaining = {(x, y, av, bv) for x, y, av, bv in pts}
    boxes = []
    while remaining:
        seed = remaining.pop()
        box = [seed]
        stack = [seed]
        while stack:
            cx, cy, av, bv = stack.pop()
            for (x, y, a2, b2) in list(remaining):
                if abs(x-cx) <= gap and abs(y-cy) <= gap:
                    remaining.remove((x, y, a2, b2))
                    box.append((x, y, a2, b2))
                    stack.append((x, y, a2, b2))
        xs = [p[0] for p in box]; ys = [p[1] for p in box]
        # a vanish = pixel had a visible sprite colour, then became backdrop
        vanish = sum(1 for p in box if p[3] == p[2])  # placeholder
        boxes.append((min(xs), min(ys), max(xs), max(ys), len(box)))
    boxes.sort(key=lambda b: -b[4])
    return boxes

def main(path, start, count):
    want = set(range(start, start+count))
    frames = parse(path, want)
    frames.sort()
    print(f"{len(frames)} frames {frames[0][0]}..{frames[-1][0]}")
    # sprite height probe: from the palette snapshot, NES $2000 bit5 isn't in RAM,
    # so we infer 8x16 by checking vertical sprite runs in the picture instead.
    for i in range(len(frames)-1):
        f0, _p0, oam0, idx0 = frames[i]
        f1, _p1, oam1, idx1 = frames[i+1]
        pts = changed_pixels(idx0, idx1)
        boxes = cluster(pts)
        # count sprites present in OAM at frame f0 overlapping each box
        sp0 = [(j, oam0[j*4], oam0[j*4+2], oam0[j*4+3]) for j in range(64) if oam0[j*4] < 0xEF]
        print(f"\nframes {f0}->{f1}: {len(pts)} changed px, {len(boxes)} clusters")
        for (x0, y0, x1, y1, n) in boxes[:6]:
            # which sprites cover this box (8x8 and 8x16 both tried)
            cov8 = [s for s in sp0 if not (s[1]+8 < y0-1 or s[1]-1 > y1 or s[3]+8 < x0-1 or s[3]-1 > x1)]
            cov16 = [s for s in sp0 if not (s[1]+16 < y0-1 or s[1]-1 > y1 or s[3]+8 < x0-1 or s[3]-1 > x1)]
            print(f"   box x{x0}-{x1} y{y0}-{y1} px={n}  oam8:{len(cov8)} oam16:{len(cov16)}")

if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 3)
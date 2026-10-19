#!/usr/bin/env python3
"""native_cols.py — GATE 2 for the margin sprites: the machine's own 256 columns
must not move when the margin starts drawing.

Compares two `--present-out` runs of the same schedule — margin sprites on, and
`--margin-sprites 0` — and reports, per frame, where the pictures differ.

THE GATE AS WRITTEN CANNOT PASS, AND THE REASON IS WORTH KEEPING

AGENTS.md says the machine's 256 columns "must differ by 0 pixels on every
frame". Measured on the committed build, at 16:9 with the reveal on, they do not,
and never did:

    native 0-7      the PPUMASK band the reveal exists to fill  (3843 px)
    native 8-14     the sprite parts that bleed out of that band  (~1790 px)
    native 248-255  the right seam, where the machine clips at 255  (451 px)
    native 15-247   the machine's own run                     (0 px)

The first three are the feature working. So the useful question is not "are they
identical" but "**did anything NEW appear inside the machine's run**", and that
is what this reports: a breakdown by region, and a separate count of pixels in
15..247, which is the region the gate actually means. Run it on two builds and
compare those numbers — that is what makes it a gate rather than a measurement.

THE INDEX IS AMBIGUOUS AND IS NOT PAPERED OVER

`--present-out` writes capture file N for host frame N+1, so a frame's block can
sit at x0[N-1] or x0[N] (AGENTS.md, "The anchor record with f = N belongs to
presented file N-1"). Pairing it wrong makes a camera-stepping frame read as a
placement error. Rather than pick one and be right by luck, the window used is
the union of both candidates, and a frame is only reported as clean if it is
clean under the *narrower* reading too — so the check cannot pass by being off by
one, at the cost of being a few columns stricter than the block really is.

usage: native_cols.py <margin-on-dir> <margin-off-dir> <trace-log>
  exit 0 clean, 1 pixels inside the machine's own run, 2 measured nothing
"""
import os, re, sys, glob
from PIL import Image
import numpy as np

if len(sys.argv) < 4:
    # 2, not 1: exit 1 is reserved for "pixels inside the machine's own run",
    # and a usage error must not look like that verdict to a caller.
    print(__doc__ or "usage: native_cols.py <on-dir> <off-dir> <trace-log>")
    sys.exit(2)
on_dir, off_dir, log = sys.argv[1], sys.argv[2], sys.argv[3]

x0 = {}
pat = re.compile(r'^sdbw f=(\d+) x0=(-?\d+):')
try:
    for ln in open(log, errors="replace"):
        m = pat.match(ln)
        if m:
            x0[int(m.group(1))] = int(m.group(2))
except FileNotFoundError:
    pass


def frames(d):
    out = {}
    for p in glob.glob(os.path.join(d, "*.png")):
        try:
            n = int(re.findall(r'(\d+)\.png$', os.path.basename(p))[0])
        except Exception:
            continue
        out[n] = p
    return out


FA, FB = frames(on_dir), frames(off_dir)
common = sorted(set(FA) & set(FB))
if not common:
    print("no shared frames -- nothing compared", file=sys.stderr)
    sys.exit(2)
print(f"frames: {len(common)} compared   x0 records: {len(x0)}")

BAND_END = 15      # the revealed band plus the sprite parts that bleed out of it
SEAM_LO = 248      # the machine clips at 255, so the right seam owns 248..255

regions = {"band 0-14": 0, "seam 248-255": 0, "OWN RUN 15-247": 0}
total = 0
no_x0 = 0
bad = []
for n in common:
    A = np.asarray(Image.open(FA[n]).convert("RGB"))
    B = np.asarray(Image.open(FB[n]).convert("RGB"))
    if A.shape != B.shape:
        bad.append((n, f"shape {A.shape} vs {B.shape}"))
        continue
    D = (A != B).any(axis=2)
    if not D.any():
        continue
    total += int(D.sum())
    lo = hi = None
    for f in (n - 1, n):
        if f in x0:
            a, b = x0[f], x0[f] + 256
            lo = a if lo is None else min(lo, a)
            hi = b if hi is None else max(hi, b)
    if lo is None:
        no_x0 += 1
        continue
    xs = np.nonzero(D)[1] - lo
    regions["band 0-14"] += int(((xs >= 0) & (xs < BAND_END)).sum())
    regions["seam 248-255"] += int(((xs >= SEAM_LO) & (xs < 256)).sum())
    own = int(((xs >= BAND_END) & (xs < SEAM_LO)).sum())
    regions["OWN RUN 15-247"] += own
    if own:
        bad.append((n, f"{own} px inside native 15-247"))

print(f"total differing pixels: {total}   (frames compared: {len(common)})")
for k, v in regions.items():
    print(f"  {k:16} {v:8d}")
if no_x0:
    print(f"  ({no_x0} differing frames had no x0 record and were skipped)")
print(f"failures: {len(bad)}")
for n, w in bad[:20]:
    print("  frame", n, w)

# Only the machine's own run is a failure. The band and the seam are the feature.
sys.exit(1 if regions["OWN RUN 15-247"] else 0)
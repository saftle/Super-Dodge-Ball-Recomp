#!/usr/bin/env python3
"""bounds_gate.py - nothing is drawn past the game's own bounds.

The widescreen margin may show world columns the machine's own 256 do not, but
only the ones the game can ever display. Those are the camera window: `$C3/$C4`
is the camera, clamped at `$E6AB` to `[$28,$E0]` = `[40,224]`, so the world the
machine can reach is `[40, 224+256)` = `[40,480)`. There is no per-level table.
See research.md section 25 for the disassembly.

This asserts the property on the PRESENTED FRAMES rather than trusting a
counter: every canvas column that is not one of the machine's own 256, whose
world column falls outside the bounds, must be the frame's backdrop.

    mkdir -p tmp/bg
    SDB_TRACE=1 ./build-cycle/nes_game ROM --frames 1200 \\
        --input tas/vectors/sdb_sweep.input --ram-init zeros \\
        --mods-root build-cycle/mods --widescreen 16:9 \\
        --present-out tmp/bg/s_%05d.png --present-every 1 2>tmp/bg/trace.log
    python3 tools/widescreen/bounds_gate.py tmp/bg 16:9 s 40 480 tmp/bg/trace.log 430

`from_frame` skips the boot, title and menu screens.

FIVE THINGS THIS HAS TO GET RIGHT, each of which made it report nonsense at
least once. They are listed because each was found by believing the output:

1. **GLOB, do not format.** The output prefix has three observed forms
   depending on how the capture was invoked, and the host appends its own frame
   number to whatever it was given. Two of the three are not valid format
   strings, and a wrong guess silently reads nothing -- which is how this gate
   reported a clean pass on a capture it never opened.
2. **THE SCROLL ALIGNMENT IS MEASURED, NOT ASSUMED.** A capture frame shows the
   guest frame of the same number, but this was wrong once and cost a false
   positive on 8 frames: using frame n+1's scroll put a frame whose camera was
   at 216 into the position of one at 40, which moved the margin 176 columns and
   reported 20550 px of "overrun" on a frame that is in fact exactly clean
   (0 px at the right alignment). Both alignments are therefore scanned and the
   one that fits the capture AS A WHOLE wins -- one scalar decision per capture,
   not a per-frame guess. The chosen offset is printed.
3. **THE BACKDROP IS NOT ALWAYS BLACK.** `cyc_render_color(0)` is palette entry
   0, a per-screen value; one course's is `(0,62,122)`. It is measured per
   frame from the margin being judged. Hardcoding black reported 635 of 4355
   frames as overrun when the margin was entirely backdrop.
4. **`--present-out` captures the WINDOW.** A `play_record.sh` session at
   `WindowScale 2` writes 853x480 PNGs for a 426x240 canvas (426*2+1). Assuming
   1:1 maps every column to the wrong world column. The scale comes from the
   image height.
5. **EXIT 2 ON NOTHING EXAMINED.** A gate that cannot fire is indistinguishable
   from one reporting nothing wrong, and this project has paid for that three
   times (research.md section 26). "No frames" is not a pass.

Exit codes: 0 clean, 1 ink past the bounds, 2 nothing examined / unopenable.
"""
import glob
import re
import sys
from collections import Counter

from PIL import Image

TOL = 8

# The engine's own widths, from common/nes_video_geometry.h.
WIDTH = {"16:9": 426, "21:9": 560, "32:9": 854, "stock": 256}


def scan(shots, scroll, first, last, lo, hi, offset):
    """Count margin ink past the bounds at one scroll alignment."""
    frames = violations = 0
    worst = []
    for n in range(first, last + 1):
        s = scroll.get(n + offset)
        if s is None or n not in shots:
            continue
        img = Image.open(shots[n]).convert("RGB")
        px = img.load()
        frames += 1
        scale = max(1, img.size[1] // 240)
        canvas_w = img.size[0] // scale
        native_x0 = (canvas_w - 256) // 2 * scale
        backdrop = Counter(px[x, y] for x in range(0, native_x0)
                           for y in range(0, img.size[1], 7)).most_common(1)[0][0]
        bad = 0
        # Margin columns, in CANVAS units. `native_x0` is in image pixels, so
        # the machine's run is [x0c, x0c + 256); adding a bare 256 to the pixel
        # value starts the right margin 128 columns early at scale 2 and sweeps
        # the machine's own picture into the check, which reported 61440 px of
        # "overrun" on a frame whose margins are black.
        x0c = native_x0 // scale
        for cx in list(range(0, x0c)) + list(range(x0c + 256, canvas_w)):
            wx = s + cx - x0c
            if lo - TOL <= wx < hi + TOL:
                continue                        # inside, or within tolerance
            for y in range(0, img.size[1], scale):
                for dx in range(scale):
                    if px[cx * scale + dx, y] != backdrop:
                        bad += 1
        if bad:
            violations += 1
            worst.append((bad, n, s))
    return frames, violations, worst


def main():
    if len(sys.argv) < 7:
        raise SystemExit(__doc__)
    directory, aspect, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    lo, hi = int(sys.argv[4]), int(sys.argv[5])
    log = sys.argv[6]
    first = int(sys.argv[7]) if len(sys.argv) > 7 else 0
    if aspect not in WIDTH:
        print(f"unknown aspect {aspect}; known: {', '.join(WIDTH)}")
        return 2

    scroll = {}
    for line in open(log):
        m = re.match(r"sdbc (\d+) scr=(-?\d+)", line)
        if m:
            scroll[int(m.group(1))] = int(m.group(2))

    shots = {}
    for path in glob.glob(f"{directory}/{prefix}*.png"):
        m = re.search(r"(\d+)\.png$", path)
        if m:
            shots[int(m.group(1))] = path
    if not shots:
        print(f"  no PNGs matching {directory}/{prefix}*.png")
        return 2

    last = max(shots)
    # See note 2: pick the alignment that fits the capture as a whole.
    results = {off: scan(shots, scroll, first, last, lo, hi, off)
               for off in (0, 1)}
    offset = min(results, key=lambda k: results[k][1])
    frames, violations, worst = results[offset]

    print(f"{frames} frames at {aspect}; margin columns outside world "
          f"[{lo},{hi}) must be the frame's own backdrop")
    print(f"  scroll alignment: capture frame n -> trace frame n{offset:+d}")
    print(f"  {violations} frames carry ink past the bounds")
    for bad, n, s in sorted(worst, reverse=True)[:8]:
        print(f"    frame {n} scroll {s}: {bad} px")
    if frames == 0:
        print("  NO FRAMES EXAMINED -- that is a broken instrument, not a pass")
        return 2
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
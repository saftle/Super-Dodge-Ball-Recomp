#!/usr/bin/env python3
"""input_to_tas.py - a recomp `.input` vector to the canonical TAS json.

`tas/vectors/*.input` are the project's own reproduction vectors, written in the
format `nes_game --input` takes: one "<frame> <BUTTONS>" line per state change,
absolute state, `-` for release. The TAS oracle in `tools/tas/` wants the
canonical json instead — a flat per-frame button-mask array — and until now the
only converters were `bk2_to_tas.py` and `fm2_to_tas.py`, for speedrun movies.
That left the oracle unable to replay a vector, which is backwards: the vectors
are the runs we most want an oracle for.

    python3 tools/tas/input_to_tas.py tas/vectors/sdb_bar.input \\
        --out tas/vectors/sdb_bar.tas.json --frames 1200
    python3 tools/tas/run_tas.py --tas tas/vectors/sdb_bar.tas.json \\
        --frames 1200 --out tas/runs/bar1200

The mask map is the one the rest of the harness already uses, and is copied into
the json so the file is self-describing:

    A=0x80 B=0x40 SELECT=0x20 START=0x10 UP=0x08 DOWN=0x04 LEFT=0x02 RIGHT=0x01

Two details that matter and are easy to get wrong:

  - The two schedulers count frames differently. `nes_game --input` applies a
    step at the start of the named frame; `mesen_tas.lua` counts frame 0 as the
    first post-power frame and indexes the array with it. Both are 0-based from
    power-on, so the arrays line up frame for frame — but the vector's own
    comment ("frame 0 = first post-power frame") is the contract being relied on,
    so `--offset` is exposed for anything that was written against a different
    convention rather than silently guessed.
  - A state-change vector must be *held* to the next line. A vector that lists
    `<frame> LEFT` with nothing after it means one frame of LEFT, not "LEFT until
    something else says otherwise", and this converter reproduces that exactly:
    it expands each state change into the frames it covers and pads the tail with
    the last state.
"""
import argparse
import json
import os
import sys

MASK = {
    "A": 0x80, "B": 0x40, "SELECT": 0x20, "START": 0x10,
    "UP": 0x08, "DOWN": 0x04, "LEFT": 0x02, "RIGHT": 0x01,
}
MASK_MAP = "A=0x80 B=0x40 SELECT=0x20 START=0x10 UP=0x08 DOWN=0x04 LEFT=0x02 RIGHT=0x01"


def parse(path):
    """[(frame, mask)] in file order, with '#' comments and blanks dropped."""
    steps = []
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 2:
                raise SystemExit(f"{path}:{lineno}: expected '<frame> <BUTTONS>', got {line!r}")
            try:
                frame = int(parts[0])
            except ValueError:
                raise SystemExit(f"{path}:{lineno}: frame is not a number: {parts[0]!r}")
            buttons = parts[1]
            if buttons == "-":
                mask = 0
            else:
                mask = 0
                for name in buttons.split("+"):
                    key = name.strip().upper()
                    if key not in MASK:
                        raise SystemExit(f"{path}:{lineno}: unknown button {name!r}; "
                                         f"known: {', '.join(sorted(MASK))}")
                    mask |= MASK[key]
            steps.append((frame, mask))
    steps.sort(key=lambda s: s[0])
    return steps


def expand(steps, frames, offset=0):
    out = [0] * frames
    state = 0
    i = 0
    for f in range(frames):
        # every step whose frame has arrived by now, last one wins
        while i < len(steps) and steps[i][0] + offset <= f:
            state = steps[i][1]
            i += 1
        out[f] = state
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="a tas/vectors/*.input file")
    ap.add_argument("--out", required=True, help="where to write the canonical json")
    ap.add_argument("--frames", type=int, default=1200, help="how many frames to expand")
    ap.add_argument("--offset", type=int, default=0,
                    help="frames to shift every step by (default 0, see the module docstring)")
    a = ap.parse_args()

    steps = parse(a.input)
    frames = expand(steps, a.frames, a.offset)
    doc = {
        "source": os.path.basename(a.input),
        "rom_md5_headerless": "6e44871b462ce90c99d9a9a8fff637a8",
        "mask_map": MASK_MAP,
        "frames": frames,
    }
    with open(a.out, "w") as f:
        json.dump(doc, f)
    nz = sum(1 for m in frames if m)
    print(f"wrote {a.out}: {len(frames)} frames, {len(steps)} state changes, "
          f"{nz} frames with a button held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
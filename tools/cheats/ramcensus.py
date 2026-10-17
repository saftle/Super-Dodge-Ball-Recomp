#!/usr/bin/env python3
"""ramcensus.py - which RAM bytes behave like a counter, over a --frame-log.

WHY THIS EXISTS
---------------
Looking for "the byte that advances the stage" has no shortcut: there is no
stage-crossing vector in tas/vectors/, so the address has to come out of a
recorded run. A --frame-log already carries CPU RAM for every frame, so the
question is not how to collect it but how to read 2048 columns of 2600 rows
without eyeballing a diff. This answers it: score every byte by how much it
looks like a counter and print the ones that scored.

WHAT COUNTS AS A COUNTER
------------------------
Not "changed". RAM is full of bytes that change -- timers, health bars, cursor
positions, animation frames. What a stage index or a score looks like:

  * it takes SMALL values, so the number of distinct values is far below 256;
  * it changes rarely relative to the frames (a stage index changes a handful
    of times in 2600 frames; a timer changes every frame);
  * it very often moves BY ONE, which is the signature of a counter rather than
    of a value being recomputed from something else.

The third test is what separates a real counter from a small-valued flag that
merely toggles, and it is the one that used to be missing: a byte with two
values, alternating, scores well on "small value set" and on "changes rarely",
and only the by-one test throws it out. So the score is deliberately built so a
toggle cannot win it.

A byte can also be a counter that never wraps in this window, and one that is
reset often (a per-shot score) looks identical from here. That is what --all is
for, and it is why the shortlist is a shortlist and not an answer.

USAGE
    python3 tools/cheats/ramcensus.py LOG [--from N] [--to N] [--top N] [--all]

Exit 0 always: this is an instrument, not a gate. It reads RAM only - it never
touches the picture, the cycle count or the guest - so a run of it measures
nothing about the machine except what these bytes did.
"""
import argparse
import collections
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "widescreen"))
from cycframe import records  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--from", dest="frm", type=int, default=0)
    ap.add_argument("--to", type=int, default=1 << 30)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--all", action="store_true",
                    help="every byte that changes at all, not just the shortlist")
    args = ap.parse_args()

    frames = [(r.frame, r.ram) for r in records(args.log, want=("frame", "ram"))
              if r.frame is not None and r.ram
              and args.frm <= r.frame <= args.to]
    if not frames:
        print(f"no frames in {args.log} over [{args.frm},{args.to}] "
              f"- a broken log and an empty window look the same here")
        return
    lo, hi = frames[0][0], frames[-1][0]
    print(f"{len(frames)} frames, ${lo}..${hi}, "
          f"{len(frames[0][1])} RAM bytes each")

    size = len(frames[0][1])
    results = []
    for addr in range(size):
        series = [ram[addr] for _, ram in frames]
        distinct = set(series)
        if args.all:
            if len(distinct) == 1:
                continue
        elif len(distinct) < 2 or len(distinct) > 24:
            continue

        steps = collections.Counter()
        changes = 0
        prev = series[0]
        for v in series[1:]:
            if v != prev:
                changes += 1
                delta = v - prev
                steps[(delta > 0) - (delta < 0)] += 1
                prev = v
        if changes == 0:
            continue
        by_one = steps[1]
        n = len(frames)
        results.append((
            by_one / changes,              # fraction of moves that are +1
            -changes / n,                  # rarer is better
            addr, distinct, changes, by_one, steps[-1], steps[1],
            series[0], series[-1]))

    # Rank by how counter-like, then by rarity. Both are wanted: a byte that
    # steps by one every frame is a frame timer, not a stage index.
    results.sort(key=lambda r: (round(r[0], 2), r[1]))
    top = results if args.all else results[:args.top]

    print(f"\n{'addr':>6} {'+1':>4} {'-1':>4} {'up':>4} {'dn':>4} "
          f"{'distinct':>9} {'moves':>6}  {'first':>6} {'last':>6}  series")
    for ratio, _, addr, distinct, changes, by_one, down, up, first, last in top:
        series = [ram[addr] for _, ram in frames]
        # Show where it moved, capped: a counter's whole value sequence is the
        # evidence, and 2600 frames of it is not.
        marks = [f"{series[0]:02X}"]
        prev = series[0]
        for i, v in enumerate(series[1:], 1):
            if v != prev:
                marks.append(f"@{lo + i}:{v:02X}")
                prev = v
                if len(marks) > 9:
                    marks.append("...")
                    break
        print(f"${addr:04X} {by_one:>4} {down:>4} {up:>4} {down:>4} "
              f"{len(distinct):>9} {changes:>6}  {first:02X}     {last:02X}     "
              + " ".join(marks))


if __name__ == "__main__":
    main()
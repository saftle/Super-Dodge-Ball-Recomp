#!/usr/bin/env python3
"""tas.json -> recomp --script (edge-triggered HOLD/RELEASE/WAIT).

Usage:
  python3 tools/tas/emit_script.py tas/sdb_4976.tas.json out.script [--frames N] [--start 0]

--script semantics (runner/src/input_script.c): WAIT n advances n emulated
frames; HOLD/RELEASE change held set. Input on TAS frame f must be held
during frame f. Emitter walks frames, diffs consecutive masks, coalesces
runs with WAIT. Frame 0 (power-on marker) is skipped via initial WAIT 1.
MAX_CMDS 4096 — long TAS needs chunking (see run_tas.py); this emitter warns.
"""
import json
import sys

NAMES = [(0x80, "A"), (0x40, "B"), (0x20, "SELECT"), (0x10, "START"),
         (0x08, "UP"), (0x04, "DOWN"), (0x02, "LEFT"), (0x01, "RIGHT")]


def emit(frames):
    cmds = []
    pending_wait = 0
    prev = 0
    # frame 0 = power-on: consume without input
    cmds.append("WAIT 1")
    for m in frames[1:]:
        newly = m & ~prev
        gone = prev & ~m
        prev = m
        if newly or gone:
            if pending_wait:
                cmds.append(f"WAIT {pending_wait}")
                pending_wait = 0
            for bit, name in NAMES:
                if newly & bit:
                    cmds.append(f"HOLD {name}")
            for bit, name in NAMES:
                if gone & bit:
                    cmds.append(f"RELEASE {name}")
        else:
            pending_wait += 1
    if pending_wait:
        cmds.append(f"WAIT {pending_wait}")
    # Tail: runner exits when the script completes, so outlive any --smoke count.
    cmds.append("WAIT 1000000")
    return cmds


def main():
    src = sys.argv[1]
    dst = sys.argv[2]
    maxf = None
    if "--frames" in sys.argv:
        maxf = int(sys.argv[sys.argv.index("--frames") + 1])
    tas = json.load(open(src))
    frames = tas["frames"][:maxf] if maxf else tas["frames"]
    cmds = emit(frames)
    with open(dst, "w") as f:
        f.write(f"# generated from {src} ({len(frames)} frames)\n")
        f.write("\n".join(cmds) + "\n")
    print(f"wrote {dst}: {len(cmds)} commands for {len(frames)} frames")
    if len(cmds) > 4096:
        print(f"WARNING: {len(cmds)} > MAX_CMDS 4096 — chunk this TAS for --script runs")


if __name__ == "__main__":
    main()

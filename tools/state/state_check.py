#!/usr/bin/env python3
"""state_check.py - did the replay from the anchor reproduce the played run?

THE GATE
--------
A save state that pins the RNG is only worth something if a headless replay
loaded from it lands on the same machine as the windowed run it came from. The
anchor helper took two states out of the window -- one at the anchor frame and
one `--probe` frames later -- and you replayed from the first with
`--save-state probe_mem_frame`, producing the second's twin. So the question is
one comparison with nothing to interpret: are these two files the same machine?

WHY IT WALKS THE CHUNKS
-----------------------
Comparing the files as flat byte strings answers "are they the same" and not
"what is not the same", and this state is half a megabyte of which 132 bytes are
audio. The first version of this printed "1112 of 449508 bytes differ" and left
the reader to work out from a run of offsets that the difference was scattered
across RAM, CIRAM and the PPU. A save state is a tag/length chunk sequence
(`cyc_state.c`), so the same comparison can name the chunk every difference
falls in, which is the difference between "these two files differ" and "these
two MACHINES differ".

RAM is deliberately NOT read out of the `HW  ` chunk: it sits at the offset of
the `ram` member of `HwMachine`, so pulling it out means depending on a C
struct's layout, and a tool that reported a RAM diff after the layout moved
would be reporting a lie about the machine.

THE APU IS REPORTED AND NOT FATAL
---------------------------------
Measured: the `APU  ` chunk differs by ~20 bytes on EVERY frame after the
anchor, including the first, while every other chunk is identical there. It is a
standing offset, not a drift -- and `--wav-out` on the replay does not remove it
(1112 -> 1108 bytes at the 120-frame probe), so it is not the audio mixing path
either. It is also the one chunk no measurement in this project reads, and the
one divergence measured here does not track it: the guest is identical at +3
frames with the APU differing, and diverged by +9 with the APU differing no more
than at +3.

That is an argument about scope, not a claim of equivalence: this gate is
"is the replay the same machine", and the machine means CPU, RAM, PPU and the
mod records. The APU number is printed on every run so it is never invisible,
and a change in it is still visible.

WHAT IT IS NOT
--------------
Not a proof that the artifact reappeared, and not a bound on how long an anchor
holds -- AGENTS.md has the measurement for that. It reads no pixels. It proves
the replay is the same machine at one frame, which is the thing every
measurement taken at a replay depends on and the thing AGENTS.md says is missing
("a headless replay of a recorded vector is a DIFFERENT machine from frame 4").

Usage:

    # 1. replay from the anchor. play_record.sh prints this with the frames
    #    filled in; --save-state comes from anchor.json and is the WINDOW frame
    #    minus one (see the off-by-one in state_anchor.py).
    ./build-cycle/nes_game "roms/Super Dodge Ball (USA).nes" \\
        --load-state tmp/<name>/state.bin --frames <total> \\
        --input tmp/<name>/repro.input --ram-init zeros \\
        --mods-root tmp/<name>/mods --widescreen 16:9 \\
        --save-state <probe_frame - 1>:tmp/<name>/replay.bin

    # 2. ask
    python3 tools/state/state_check.py tmp/<name>

Exit 0 the machine matches, 1 it does not, 2 examined nothing. 2 is the one to
read carefully: a missing anchor.json, an unwritten replay, or a `--save-state`
frame that never fired all produce it, and a tool that reports no difference
without looking is the failure this project keeps paying for.
"""
import argparse
import json
import os
import struct
import sys

HEADER = 32
#: Offset of Header.frame: char magic[8] then u32 version, prg_hash, cart_hash,
#: layout, program_crc (cyc_state.c). Named because a frame-index mistake is the
#: one difference here that is not about the machine.
FRAME_OFFSET = 28

#: The machine: CPU, RAM (inside HW), the cartridge, the PPU and the picture the
#: PPU produced this frame. A difference in any of these is a different machine.
GUEST = {"CPU", "HW", "CART", "PPU", "PICI", "PICL", "PICB"}
#: This project's own presentation mods' records, which a save state carries.
#: Not the guest, but not the host either: a difference here is ours.
MODS = {"MOD"}


def frame_of(path):
    with open(path, "rb") as fh:
        head = fh.read(FRAME_OFFSET + 4)
    if len(head) < FRAME_OFFSET + 4:
        return None
    return int.from_bytes(head[FRAME_OFFSET:FRAME_OFFSET + 4], "little")


def chunks(blob):
    """[(tag, body_offset, length)] over a CYCSTATE file's tag/length chunks."""
    off = HEADER
    while off + 8 <= len(blob):
        tag = blob[off:off + 4].decode("latin1").strip()
        (ln,) = struct.unpack_from("<I", blob, off + 4)
        body = off + 8
        yield tag, body, ln
        off = body + ln
        if tag == "END":
            break


def diff_by_chunk(played, replay):
    """{tag: differing bytes} plus the list of (tag, first_offset) runs."""
    per = {}
    for tag, body, ln in chunks(played):
        per[tag] = sum(1 for i in range(body, min(body + ln, len(replay)))
                       if played[i] != replay[i])
    return per


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", help="the capture directory (holds anchor.json)")
    ap.add_argument("--replay", default=None,
                    help="the replay's --save-state file (default <dir>/replay.bin)")
    args = ap.parse_args()

    ajson = os.path.join(args.dir, "anchor.json")
    if not os.path.exists(ajson):
        print(f"state_check: no {ajson}. The anchor never completed -- see "
              f"{args.dir}/anchor.log.", file=sys.stderr)
        return 2
    with open(ajson) as fh:
        rec = json.load(fh)

    if not rec.get("probed"):
        print(f"state_check: anchor frame {rec.get('anchor_frame')} was saved "
              f"but never probed, so there is nothing to compare against. "
              "state_anchor.py stops at exit 3 when the run ended first; "
              "re-capture with the probe frame inside the run.", file=sys.stderr)
        return 2

    played_path = os.path.join(args.dir, rec["probe"])
    replay_path = args.replay or os.path.join(args.dir, "replay.bin")

    if not os.path.exists(played_path):
        print(f"state_check: {played_path} is missing, though anchor.json claims "
              "it. The capture directory has been mixed up.", file=sys.stderr)
        return 2
    if not os.path.exists(replay_path):
        print(f"state_check: no replay state at {replay_path}. The command to "
              "produce it, with the frames from anchor.json:", file=sys.stderr)
        print(f"    --load-state {os.path.join(args.dir, rec['state'])} \\\n"
              f"    --save-state {rec['probe_mem_frame']}:{replay_path}",
              file=sys.stderr)
        print("  (--save-state takes the WINDOW frame MINUS ONE: the window counts"
              " frames completed, the headless host saves after a frame ran.\n"
              "   state_anchor.py works that out once; this reads its answer.)",
              file=sys.stderr)
        return 2

    played = open(played_path, "rb").read()
    replay = open(replay_path, "rb").read()
    fa, fb = frame_of(played_path), frame_of(replay_path)
    # The ACHIEVED gap, which is not the one that was asked for: writing a state
    # costs the window frames, and the second save lands later than the first.
    gap = rec.get("probe_gap", rec["probe_frame"] - rec["anchor_frame"])

    if fa != fb:
        print(f"state_check: WRONG FRAME -- the played state is frame {fa}, the "
              f"replay's is {fb}. Nothing below is about the machine: compare two "
              "different frames and the tool is the bug, not the replay.")
        print(f"             anchor.json says probe_mem_frame={rec['probe_mem_frame']}"
              f" for a window probe frame of {rec['probe_frame']}.")
        return 1

    if len(played) != len(replay):
        print(f"state_check: DIFFERENT SIZE -- played {len(played)} bytes, replay "
              f"{len(replay)}.")
        print("             A section is present on one side and not the other, so "
              "the two runs were not configured alike (a different aspect, or "
              "--ppu-overclock on one side).")
        return 1

    per = diff_by_chunk(played, replay)
    apu = per.get("APU", 0)
    guest = sum(v for k, v in per.items() if k in GUEST)
    mods = sum(v for k, v in per.items() if k in MODS)
    other = sum(v for k, v in per.items()
                if k not in GUEST and k not in MODS and k != "APU")
    total = sum(per.values())
    ok = not (guest or mods or other)

    print(f"state_check: {'MATCHES' if ok else 'DRIFTED'} -- frame {fa}, "
          f"{gap} frames past the anchor at {rec['anchor_frame']}, "
          f"{len(played)} bytes")
    print(f"             guest chunks {guest}   mod records {mods}   "
          f"other {other}   APU {apu}   total {total}")
    moved = [k for k, v in per.items() if v]
    if moved:
        print(f"             differing chunks: {' '.join(f'{k}={per[k]}' for k in moved)}")

    if apu and ok:
        print(f"             The APU's {apu} bytes differ and nothing else does. That is "
              "measured, not")
        print("             waived: it is a standing offset from the anchor frame itself,")
        print("             and --wav-out on the replay does not remove it.")

    if ok:
        print("             The replay IS the played machine at this frame: CPU, RAM,")
        print("             PPU and the mods' records all match byte for byte, so the")
        print("             game's RNG -- ordinary guest RAM -- is pinned, not merely")
        print("             hoped to line up.")
        return 0

    print()
    print("             The GUEST differs, so the replay is no longer the played run")
    print("             and nothing measured at it can be trusted. Two things this is")
    print("             NOT, because both are the mistakes this instrument is here to")
    print("             catch:")
    print("               - not a wrong frame: both states record frame %d above." % fa)
    print("               - not a broken anchor: a save state IS the machine, so the")
    print("                 load put the replay exactly on this run's state, and the")
    print("                 guest matched byte for byte at +3 (AGENTS.md).")
    print("             It is the windowed and headless machines parting company again")
    print("             past the anchor, which AGENTS.md records as open and")
    print("             unattributed. --wav-out does not change it, so it is not the")
    print("             audio path either.")
    print("             And it is NOT PREDICTABLE from the frame count: measured at +9,")
    print("             a scripted vector had drifted by 85 guest bytes and a played")
    print("             session had drifted by ZERO. How far an anchor holds depends on")
    print("             what the guest is doing when the two machines part company, so")
    print("             read this as a measurement of ONE run, not a verdict on the")
    print("             anchor.")
    print("             Lowering --probe will NOT reliably get you a MATCHES: two 450 KB")
    print("             states cannot be taken closer than about 6 frames apart on a")
    print("             16.6 ms host loop, so a probe cannot reach the +3 window at all.")
    print("             This tool measures the drift; it cannot reach it.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
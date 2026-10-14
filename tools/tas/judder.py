#!/usr/bin/env python3
"""judder.py - attribute the pan judder to the game or to this engine.

RESOLVED 2026-10-13: it is the game's own cadence. See research.md section 27.
This script is kept so the answer is re-derivable in one command rather than
re-chased, which is what section 27 asked for.

    python3 tools/tas/run_tas.py --tas tas/vectors/sdb_bar.tas.json \\
        --frames 700 --out tas/runs/bar700
    python3 tools/tas/judder.py tas/runs/bar700

THE METRIC, and why it needs no picture correlation
--------------------------------------------------
While the camera pans, the player walks left one 6-7 px step and the camera
walks right one 6-7 px step. If the game does both on the SAME frame they
cancel and the player is glued to the court. If they are `N` frames apart, the
player detaches from the floor for `N` frames and snaps back -- which is
exactly what is seen.

So define, for the player sprite (OAM y=143, tile $20):

    world_x(frame) = OAM X + $C3/$C4

and measure N, the number of frames between a camera step and the next sprite
step. world_x sits on a base value and blips by one step for N frames.

Both quantities are in artefacts each side already writes -- the cycle log
carries RAM and OAM, and MesenCE dumps `.ram.bin` and `.oam.bin` per frame at
SHOT_STRIDE 1 -- so no picture analysis and no new emulator probe is needed.
An earlier attempt compared frames by column-SAD and locked onto flat regions
of the court; do not do that.

MEASURED
--------
    cycle backend    camera 601,608,615,622   sprite 603,610,617,624   N = 2,2,2,2
    MesenCE 2.2.1    camera 603,610,617        sprite 605,612            N = 2,2

Same N, same base (207), same blip. The game updates the camera and the sprite
OAM two frames apart, and a second, independent emulator reproduces it exactly.
Nothing to fix, and specifically not in this engine's PPU model -- which is why
it must not be smoothed in a mod: the judder is in the machine's own 256-column
picture, which the compositor's contract is to reproduce faithfully.

The two sides log on different events (cycle via `--frame-log-at mesen`,
MesenCE via `endFrame`), so absolute frames are offset by a constant. N is a
DIFFERENCE of frames and compares directly; the frame numbers do not.
"""
import collections
import glob
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "widescreen"))
from cycframe import records                                    # noqa: E402

CAM = 0x00C3          # camera, $C3/$C4, little-endian
OAM_Y, OAM_TILE = 143, 0x20
LO, HI = 590, 630
NATIVE_X = 256


def _cycle(root):
    rs = {r.frame: r for r in records(os.path.join(root, "cyc", "frames.bin"),
                                      want=("frame", "ram", "oam"))}
    def oam(f):
        return rs[f].oam if f in rs else None
    def cam(f):
        ram = rs[f].ram
        return ram[CAM] | (ram[CAM + 1] << 8)
    return oam, cam


def _mesen(root):
    d = os.path.join(root, "mesen")
    def oam(f):
        p = os.path.join(d, f"mesen_{f:05d}.oam.bin")
        return open(p, "rb").read() if os.path.exists(p) else None
    def cam(f):
        ram = open(os.path.join(d, f"mesen_{f:05d}.ram.bin"), "rb").read()
        return ram[CAM] | (ram[CAM + 1] << 8)
    return oam, cam


def sprite_series(oam_of, cam_of):
    """(frame, cam, x) where exactly one OAM slot holds the tracked sprite.

    The game rebuilds OAM every frame and re-assigns slots, so matching is on
    (y, tile) rather than on slot index -- a slot's contents are reassigned
    between builds. Requiring exactly one match skips the frames where the
    sprite is mid-rebuild or duplicated, which is the honest thing to do rather
    than guess which of two candidates is "the" player.
    """
    out = []
    for f in range(LO, HI + 1):
        oam = oam_of(f)
        if oam is None or len(oam) < NATIVE_X:
            continue
        hits = [oam[j * 4 + 3] for j in range(64)
                if oam[j * 4] == OAM_Y and oam[j * 4 + 1] == OAM_TILE
                and oam[j * 4 + 3] < 0xEF]
        if len(hits) == 1:
            out.append((f, cam_of(f), hits[0]))
    return out


def measure(name, series):
    if len(series) < 8:
        print(f"{name}: only {len(series)} frames carried exactly one such "
              f"sprite; not enough to judge")
        return None
    cam_s, x_s, pc, px = [], [], None, None
    for f, cam, x in series:
        if pc is not None:
            if cam != pc:
                cam_s.append(f)
            if x != px:
                x_s.append(f)
        pc, px = cam, x
    if not cam_s or not x_s:
        print(f"{name}: no steps in the window")
        return None
    offs = [xf - max(cf for cf in cam_s if cf <= xf)
            for xf in x_s if any(cf <= xf for cf in cam_s)]
    world = [x + c for _f, c, x in series]
    base = collections.Counter(world).most_common(1)[0][0]
    blips = sum(1 for v in world if v != base)
    print(f"\n=== {name}  ({series[0][0]}..{series[-1][0]}, {len(series)} frames) ===")
    print(f"  camera steps at : {cam_s}")
    print(f"  sprite  steps at : {x_s}")
    if offs:
        c = collections.Counter(offs)
        print(f"  N (sprite step N frames after a camera step): {offs} -> {dict(c)}")
    print(f"  world_x: base {base}, {len(set(world))} distinct values, "
          f"blipping on {blips} of {len(world)} frames")
    return collections.Counter(offs).most_common(1)[0][0] if offs else None


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    root = sys.argv[1]
    if not os.path.isdir(root):
        raise SystemExit(f"{root}: not a directory. See the usage above.")
    results = {}
    for name, make in (("cycle backend", _cycle), ("MesenCE", _mesen)):
        if name == "cycle backend":
            if not os.path.exists(os.path.join(root, "cyc", "frames.bin")):
                print(f"{root}: no cyc/frames.bin, skipping {name}")
                continue
        elif not glob.glob(os.path.join(root, "mesen", "mesen_*.oam.bin")):
            print(f"{root}: no mesen/*.oam.bin, skipping {name}")
            continue
        oam_of, cam_of = make(root)
        results[name] = measure(name, sprite_series(oam_of, cam_of))
    if len(results) == 2 and len(set(results.values())) == 1:
        print(f"\nBOTH SIDES MEASURE N = {results['cycle backend']}: the game "
              f"updates the camera and the sprite OAM that far apart. "
              f"The judder is the game's own cadence, not this engine. "
              f"(research.md 27)")
        return 0
    if len(results) == 2:
        print(f"\nN DIFFERS: {results}. That is an engine difference and "
              f"belongs upstream as a PPU patch beside patches/006.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
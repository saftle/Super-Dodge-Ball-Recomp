#!/usr/bin/env python3
"""objects.py - the game's off-screen sprite cull, read out of a --frame-log.

Backs the research.md section 26 claim that Super Dodge Ball computes every
sprite's position in a 16-bit SCREEN coordinate and then discards anything
outside [4, 255], which is what makes the widescreen margins able to hold
sprites at all.

    $8348 (the per-object strip walk):
        $8470: LDA $04,X / CLC / ADC $0A,X / STA $04,X   (twice: X and Y)
        $8483: <-- the capture site: the 16-bit position is final here
        $8488: LDA $04 / CMP #$04 / BCC $849A    X < 4          -> hide
        $848E: LDA $06 / CMP #$04 / BCC $849A    Y < 4          -> hide
        $8494: LDA $05 / ORA $07 / BEQ $84A1     a high byte set -> hide
        $849A: LDA #$F8 / STA $0200,X            hide (Y = $F8)
        $84A1: LDA $04 / SEC / SBC #$04 / STA $0203,X   OAM X = X16 - 4
        $84A9: LDA $06 / CLC / ADC #$04 / EOR #$FF / SEC / SBC #$10
              STA $0200,X                               OAM Y = $EB - Y16

    So OAM X is a BYTE of a 16-bit quantity, and the truncation is what loses
    the sprite. Everything needed to place it (tile, flips, palette, and both
    halves of the position) is in the accumulator one instruction earlier.

The same cull exists on the single-sprite path at $8302, reached from $8294
(record bit 6), $82B9 and $82DD (bit 4), which share the accumulators but take
the tile from $02/$03 rather than the OAM slot.

What this needs, and what it cannot do: OAM X is the only place the game keeps
a resolved sprite column, and it is 8 bits. This script therefore verifies the
cull from the RAM around it rather than reading positions that were thrown
away -- see --cull, which reports the camera, the object records' 16-bit X and
how many of them fall outside the viewport.

    nes_game ROM --frames 700 --input tas/vectors/sdb_idle.input \\
        --ram-init zeros --frame-log tmp/ws/objs.bin --frame-log-frames 600:640
    python3 tools/widescreen/objects.py tmp/ws/objs.bin --cull
    python3 tools/widescreen/objects.py tmp/ws/objs.bin --records 605
"""
import struct
import sys

REC_BASE = 0x0618       # display record, 7 bytes per object
REC_STRIDE = 7
CAMERA = 0x00C3         # $C3/$C4, the camera $821E subtracts from bytes 3-4
LIST_CNT = 0x067A
LIST_ID = 0x067C
CURSOR = 0x0617


def records(path):
    """Yield (frame, ram, oam, ciram, picture) from a CYCFRAME v2 log."""
    with open(path, "rb") as f:
        if f.read(8) != b"CYCFRAME":
            raise SystemExit("not a CYCFRAME log")
        f.read(4)
        while True:
            head = f.read(36)
            if len(head) < 36:
                return
            (fr, _, _, l_ram, l_ciram, l_cart, l_chr, l_pic, l_aud) = struct.unpack("<9I", head)
            f.read(32)
            oam = f.read(256)
            ram = f.read(l_ram)
            ciram = f.read(l_ciram)
            f.read(l_cart)
            f.read(l_chr)
            pic = f.read(l_pic)
            f.read(l_aud)
            yield fr, ram, oam, ciram, pic


def obj_records(ram):
    """Every display record, as (index, state, sel, rel16, abs16).

    Bytes 0-2 are the state byte and two selectors; bytes 3-4 are a 16-bit
    camera-RELATIVE X ($E378 writes $45/$46, and $821E does
    `SEC / SBC $C3 / SBC $C4` on them as it loads); bytes 5-6 are a 16-bit
    absolute value. Which of the two is world X is the question -- see --cull."""
    out = []
    for i in range(13):
        r = ram[REC_BASE + REC_STRIDE * i: REC_BASE + REC_STRIDE * i + REC_STRIDE]
        out.append((i, r[0], r[1] & 0x78, r[3] | (r[4] << 8), r[5] | (r[6] << 8)))
    return out


def cull(path):
    """How many objects sit outside the 256-column viewport at each frame.

    Bytes 3-4 minus the camera is the screen column the emitter works from, so
    an object whose column is < 0 or > 255 is one the game will place and then
    hide. This is the population the margin-sprite feature draws."""
    total = outside = frames_out = 0
    per_frame = []
    for fr, ram, oam, ciram, pic in records(path):
        cam = ram[CAMERA] | (ram[CAMERA + 1] << 8)
        n = o = 0
        for (i, st, sel, rel, ab) in obj_records(ram):
            if st == 0:
                continue                      # unused record
            n += 1
            x = rel - cam
            if x < 4 or x > 255:
                o += 1
        total += n
        outside += o
        if o:
            frames_out += 1
        per_frame.append((fr, n, o))
    if not total:
        print("no populated records in this range")
        return
    print(f" frames with at least one object off-screen: {frames_out}/{len(per_frame)}")
    print(f" objects off-screen: {outside}/{total} ({100.0*outside/total:.1f}%)")
    top = sorted(per_frame, key=lambda t: -t[2])[:8]
    print(" worst frames (frame, objects, off-screen): " +
          ", ".join(f"{a}:{b}/{c}" for a, b, c in top))


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    path = args[0]
    if "--cull" in args:
        cull(path)
        return
    only = int(args[args.index("--records") + 1]) if "--records" in args else None
    for fr, ram, oam, ciram, pic in records(path):
        if only is not None and fr != only:
            continue
        cam = ram[CAMERA] | (ram[CAMERA + 1] << 8)
        print(f"=== frame {fr}: camera {cam}, cursor ${CURSOR:02X}, "
              f"list count {ram[LIST_CNT]} ===")
        print(" list ids: " + " ".join(f"{ram[LIST_ID + 2*i]:02X}" for i in range(ram[LIST_CNT])))
        print(" idx  state sel   rel16  abs16   screenX  off-screen")
        for (i, st, sel, rel, ab) in obj_records(ram):
            if st == 0:
                continue
            x = rel - cam
            flag = "YES" if (x < 4 or x > 255) else ""
            print(f" {i:3d}  ${st:02X}  {sel:02X}  {rel:5d}  {ab:5d}   {x:7d}   {flag}")
        used = [(i, oam[i*4], oam[i*4+1], oam[i*4+2], oam[i*4+3])
                for i in range(64) if oam[i*4] < 0xEF]
        print(f" OAM: {len(used)} of 64 slots in use "
              f"(the {64-len(used)} others are Y=$F0/$F8, i.e. the game's hidden ones)")
        return


if __name__ == "__main__":
    main()
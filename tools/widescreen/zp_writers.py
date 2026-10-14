#!/usr/bin/env python3
"""zp_writers.py - every instruction in the ROM that WRITES zero page.

The camera this project cares about lives at $C3/$C4 (research 26/27: the game
makes OAM X with `$823E/$8246: SEC / SBC $C3 / SBC $C4`). Finding where it is
written, and what bounds it, is the question behind "the world's own bounds" --
the margin must not invent columns the machine can never display, and only the
game knows its own limits.

This is a raw byte scan, so it reports DATA that looks like an opcode as well as
code. That is deliberate: a linear sweep cannot tell them apart, and the point
is to produce candidates to disassemble, not to be right on its own. Confirm
every hit with tools/dis65.py before believing it.

    python3 tools/widescreen/zp_writers.py $C3
"""
import sys

ROM = "roms/Super Dodge Ball (USA).nes"
PRG_BANKS = 8
BANK = 16384

# Zero-page instructions whose operand is written to: opcodes and their length.
WRITERS = {
    0x85: "STA", 0x86: "STX", 0x84: "STY",          # store zp
    0x94: "STY", 0xE6: "INC", 0xC6: "DEC",          # STY zx, INC/DEC zp
    0x0E: "ASL", 0x2E: "ROL", 0x4E: "LSR", 0x6E: "ROR",
}


def cpu_addr(off):
    """File offset -> CPU address, for this MMC1 layout."""
    base = 16
    if off < base + (PRG_BANKS - 1) * BANK:
        return 0x8000 + (off - base) % BANK
    return 0xC000 + (off - (base + (PRG_BANKS - 1) * BANK))


def bank_of(off):
    base = 16
    if off < base + (PRG_BANKS - 1) * BANK:
        return (off - base) // BANK
    return "fixed"


def main():
    targets = {int(a, 16) for a in sys.argv[1:]} or {0xC3, 0xC4}
    rom = open(ROM, "rb").read()
    body = rom[16:]
    for i in range(len(body) - 1):
        op = body[i]
        if op not in WRITERS:
            continue
        zp = body[i + 1]
        if zp not in targets:
            continue
        off = 16 + i
        print(f"${cpu_addr(off):04X}  bank {bank_of(off)}  file 0x{off:05X}  "
              f"{WRITERS[op]} ${zp:02X}")


if __name__ == "__main__":
    main()
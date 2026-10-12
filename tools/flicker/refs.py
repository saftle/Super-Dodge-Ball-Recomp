#!/usr/bin/env python3
"""refs.py — every 6502 instruction in the PRG that references an absolute address.

Linear sweep (no resync), official opcodes only. Prints bank, CPU address, the
instruction, and the opcode family. Good enough to find who reads/writes a
scratch-RAM byte when you already know the address.
"""
import sys

# opcode -> (mnemonic, mode) for absolute-addressing modes only
ABS = {
    0x8D: ("STA", "abs"), 0x9D: ("STA", "abs,X"), 0x99: ("STA", "abs,Y"),
    0xAD: ("LDA", "abs"), 0xBD: ("LDA", "abs,X"), 0xB9: ("LDA", "abs,Y"),
    0x8E: ("STX", "abs"), 0x8C: ("STY", "abs"),
    0xAE: ("LDX", "abs"), 0xBE: ("LDX", "abs,Y"),
    0xAC: ("LDY", "abs"), 0xBC: ("LDY", "abs,X"),
    0x2C: ("BIT", "abs"), 0x4C: ("JMP", "abs"), 0x6C: ("JMP", "ind"),
    0x20: ("JSR", "abs"), 0x0C: ("NOP", "abs"),
    0xEE: ("INC", "abs"), 0xCE: ("DEC", "abs"),
    0xCE: ("DEC", "abs"), 0xCC: ("CPY", "abs"), 0xEC: ("CPX", "abs"),
    0xCC: ("CPY", "abs"), 0xCD: ("CMP", "abs"), 0xDD: ("CMP", "abs,X"),
    0xD9: ("CMP", "abs,Y"), 0xED: ("SBC", "abs"), 0xFD: ("SBC", "abs,X"),
    0xF9: ("SBC", "abs,Y"), 0x0D: ("ORA", "abs"), 0x1D: ("ORA", "abs,X"),
    0x19: ("ORA", "abs,Y"), 0x2D: ("AND", "abs"), 0x3D: ("AND", "abs,X"),
    0x39: ("AND", "abs,Y"),
}
LEN = {"abs": 3, "abs,X": 3, "abs,Y": 3, "ind": 3}


def main(rom_path, addrs):
    rom = open(rom_path, "rb").read()
    want = set(int(a, 16) for a in addrs)
    for b in range(8):
        base = 16 + b * 16384
        prg = rom[base:base + 16384]
        base_cpu = 0xC000 if b == 7 else 0x8000
        i = 0
        while i < len(prg) - 2:
            op = prg[i]
            if op in ABS:
                nm, mode = ABS[op]
                addr = prg[i + 1] | (prg[i + 2] << 8)
                if addr in want:
                    print(f"bank {b}  ${base_cpu + i:04X}  ${addr:04X}  {nm:4s} {mode:5s}"
                          f"   bytes {prg[i:i+3].hex()}")
                i += LEN[mode]
                continue
            i += 1


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
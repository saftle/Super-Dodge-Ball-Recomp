#!/usr/bin/env python3
"""Minimal 6502 disassembler for ROM analysis (official opcodes only).
Usage: dis65.py <binfile> <hexaddr> <count> <fileoffset>
  binfile: ROM file; hexaddr: CPU address to print; count: instructions;
  fileoffset: byte offset in file where hexaddr lives (decimal or 0x-hex).
Linear sweep, no resync after data bytes. Verified bank-0/bank-6/fixed-bank
entries ($82BD5852 table, $A220 dispatch, $FF3A/$FF4C/$FCA0) against it."""
import sys

# opcode: (mnemonic, mode)  modes: impl, acc, imm, zp, zpx, zpy, abs, absx, absy, ind, indx, indy, rel, izi
OPS = {}
def _ops(table):
    for op, (m, md) in table.items():
        OPS[op] = (m, md)
_ops({
0x00:("BRK","impl"),0x08:("PHP","impl"),0x0A:("ASL","acc"),0x10:("BPL","rel"),
0x18:("CLC","impl"),0x20:("JSR","abs"),0x28:("PLP","impl"),0x2A:("ROL","acc"),
0x30:("BMI","rel"),0x38:("SEC","impl"),0x40:("RTI","impl"),0x48:("PHA","impl"),
0x4C:("JMP","abs"),0x50:("BVC","rel"),0x58:("CLI","impl"),0x60:("RTS","impl"),
0x68:("PLA","impl"),0x6C:("JMP","ind"),0x70:("BVS","rel"),0x78:("SEI","impl"),
0x88:("DEY","impl"),0x8A:("TXA","impl"),0x90:("BCC","rel"),0x98:("TYA","impl"),
0x9A:("TXS","impl"),0xA0:("LDY","imm"),0xA8:("TAY","impl"),0xAA:("TAX","impl"),
0xB0:("BCS","rel"),0xB8:("CLV","impl"),0xBA:("TSX","impl"),0xC8:("INY","impl"),
0xCA:("DEX","impl"),0xD0:("BNE","rel"),0xD8:("CLD","impl"),0xE8:("INX","impl"),
0xEA:("NOP","impl"),0xF0:("BEQ","rel"),0xF8:("SED","impl"),
})
for base, m in [(0x01,"ORA"),(0x21,"AND"),(0x41,"EOR"),(0x61,"ADC"),(0x81,"STA"),
                (0xA1,"LDA"),(0xC1,"CMP"),(0xE1,"SBC")]:
    OPS[base+0x08] = (m,"imm"); OPS[base+0x04] = (m,"zp"); OPS[base+0x14] = (m,"zpx")
    OPS[base+0x0C] = (m,"abs"); OPS[base+0x1C] = (m,"absx"); OPS[base+0x18] = (m,"absy")
    OPS[base+0x00] = (m,"indx"); OPS[base+0x10] = (m,"indy")
for _base, _m in [(0x02,"ASL"),(0x22,"ROL"),(0x42,"LSR"),(0x62,"ROR"),
                 (0xC2,"DEC"),(0xE2,"INC")]:
    pass  # covered explicitly below; loop kept for table symmetry
for op,(m,md) in {0x06:("ASL","zp"),0x0E:("ASL","abs"),0x16:("ASL","zpx"),0x1E:("ASL","absx"),
0x26:("ROL","zp"),0x2E:("ROL","abs"),0x36:("ROL","zpx"),0x3E:("ROL","absx"),
0x46:("LSR","zp"),0x4E:("LSR","abs"),0x56:("LSR","zpx"),0x5E:("LSR","absx"),
0x66:("ROR","zp"),0x6E:("ROR","abs"),0x76:("ROR","zpx"),0x7E:("ROR","absx"),
0xC6:("DEC","zp"),0xCE:("DEC","abs"),0xD6:("DEC","zpx"),0xDE:("DEC","absx"),
0xE6:("INC","zp"),0xEE:("INC","abs"),0xF6:("INC","zpx"),0xFE:("INC","absx"),
0x24:("BIT","zp"),0x2C:("BIT","abs"),
0x84:("STY","zp"),0x8C:("STY","abs"),0x94:("STY","zpx"),
0x85:("STA","zp"),0x8D:("STA","abs"),0x95:("STA","zpx"),0x9D:("STA","absx"),0x99:("STA","absy"),0x91:("STA","indy"),0x81:("STA","indx"),
0x86:("STX","zp"),0x8E:("STX","abs"),0x96:("STX","zpy"),
0xA2:("LDX","imm"),0xA6:("LDX","zp"),0xAE:("LDX","abs"),0xB6:("LDX","zpy"),0xBE:("LDX","absy"),
0xA0:("LDY","imm"),0xA4:("LDY","zp"),0xAC:("LDY","abs"),0xB4:("LDY","zpx"),0xBC:("LDY","absx"),
0xA5:("LDA","zp"),0xAD:("LDA","abs"),0xB5:("LDA","zpx"),0xBD:("LDA","absx"),0xB9:("LDA","absy"),
0xA9:("LDA","imm"),
0xC0:("CPY","imm"),0xC4:("CPY","zp"),0xCC:("CPY","abs"),
0xE0:("CPX","imm"),0xE4:("CPX","zp"),0xEC:("CPX","abs"),
0xC5:("CMP","zp"),0xCD:("CMP","abs"),0xC9:("CMP","imm"),
0xE5:("SBC","zp"),0xED:("SBC","abs"),0xE9:("SBC","imm"),
0x65:("ADC","zp"),0x6D:("ADC","abs"),0x69:("ADC","imm"),0x75:("ADC","zpx"),0x7D:("ADC","absx"),0x79:("ADC","absy"),
0x25:("AND","zp"),0x2D:("AND","abs"),0x29:("AND","imm"),
0x09:("ORA","imm"),0x0D:("ORA","abs"),
0x49:("EOR","imm"),0x45:("EOR","zp"),0x4D:("EOR","abs"),0x55:("EOR","zpx"),0x5D:("EOR","absx"),0x59:("EOR","absy"),
0x05:("ORA","zp"),0x15:("ORA","zpx"),0x1D:("ORA","absx"),0x19:("ORA","absy"),
0x29:("AND","imm"),
0x4A:("LSR","acc"),0x6A:("ROR","acc"),0x2A:("ROL","acc"),
0x9A:("TXS","impl"),0xBA:("TSX","impl"),0x8A:("TXA","impl"),0x9B:("SHS","impl"),0xAB:("LXA","imm"),
0x98:("TYA","impl"),0xA8:("TAY","impl"),
0xAA:("TAX","impl"),0x8A:("TXA","impl"),
0xC8:("INY","impl"),0x88:("DEY","impl"),0xE8:("INX","impl"),0xCA:("DEX","impl"),
0x18:("CLC","impl"),0x38:("SEC","impl"),0x58:("CLI","impl"),0x78:("SEI","impl"),
0xB8:("CLV","impl"),0xD8:("CLD","impl"),0xF8:("SED","impl"),
}.items():
    OPS[op] = (m, md)
SIZES = {"impl":1,"acc":1,"imm":2,"zp":2,"zpx":2,"zpy":2,"abs":3,"absx":3,"absy":3,
         "ind":3,"indx":2,"indy":2,"rel":2,"izi":2}

def dis(data, base, count):
    pc = 0
    out = []
    for _ in range(count):
        if pc >= len(data):
            break
        op = data[pc]
        addr = base + pc
        if op not in OPS:
            out.append("%04X: %02X        ??? " % (addr, op))
            pc += 1
            continue
        m, md = OPS[op]
        sz = SIZES[md]
        raw = data[pc:pc+sz]
        if len(raw) < sz:
            break
        ops = ""
        if md == "imm": ops = "#$%02X" % raw[1]
        elif md in ("zp","zpx","zpy","indx","indy"):
            ops = "$%02X" % raw[1]
            if md == "zpx": ops += ",X"
            elif md == "zpy": ops += ",Y"
            elif md == "indx": ops = "($%02X,X)" % raw[1]
            elif md == "indy": ops = "($%02X),Y" % raw[1]
        elif md in ("abs","absx","absy","ind"):
            v = raw[1] | (raw[2] << 8)
            ops = "$%04X" % v
            if md == "absx": ops += ",X"
            elif md == "absy": ops += ",Y"
            elif md == "ind": ops = "($%04X)" % v
        elif md == "rel":
            tgt = (addr + 2 + (raw[1] - 256 if raw[1] >= 128 else raw[1])) & 0xFFFF
            ops = "$%04X" % tgt
        hexb = " ".join("%02X" % b for b in raw).ljust(8)
        out.append("%04X: %s %s %s" % (addr, hexb, m, ops))
        pc += sz
        if m in ("RTS","RTI","JMP","BRK"):
            out.append("")
    return "\n".join(out)

if __name__ == "__main__":
    f = sys.argv[1]
    base = int(sys.argv[2], 16)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    foff = int(sys.argv[4], 0) if len(sys.argv) > 4 else None
    data = open(f, "rb").read()
    if foff is None:
        # assume full ROM with 16-byte header, bank = (base>=0xC000)? 7 : ?
        print("need file offset as 4th arg")
        sys.exit(1)
    print(dis(data[foff:foff+256], base, n))

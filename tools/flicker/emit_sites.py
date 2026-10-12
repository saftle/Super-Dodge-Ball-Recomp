#!/usr/bin/env python3
"""emit_sites.py — find every 6502 cursor-bump site for the OAM writer.

Super Dodge Ball builds OAM in place at $0200 with an 8-bit byte cursor $0617
(DMA source page $02, via $FE34).  Every emitter writes a complete 4-byte sprite
and then advances the cursor by 4.  Two shapes occur:

    LDA $0617 / CLC / ADC #$04 / STA $0617      (A..D)
    LDX $0617 ... STX $0617                     (X form)

We locate the byte after each cursor advance (the natural mod_function_hook
site: the complete sprite at $0200+cursor is already written) and note whether
the following BEQ bails out when the cursor wraps to zero.
"""
import struct

ROM = "roms/Super Dodge Ball (USA).nes"

def bank_of(addr):
    return 7 if addr >= 0xC000 else None  # caller supplies switchable bank

def scan_bank(bank):
    rom = open(ROM, "rb").read()
    base = 16 + bank * 16384
    prg = rom[base:base + 16384]
    base_cpu = 0xC000 if bank == 7 else 0x8000
    hits = []
    i = 0
    while i < len(prg) - 3:
        # LDA $0617 ; CLC ; ADC #$04 ; STA $0617
        if (prg[i:i+2] == b"\xAD\x17" and prg[i+2] == 0x06
                and prg[i+3] == 0x18 and prg[i+4] == 0x69 and prg[i+5] == 0x04
                and prg[i+6] == 0x8D and prg[i+7] == 0x17 and prg[i+8] == 0x06):
            cpu = base_cpu + i
            after = prg[i+9:i+11]
            bail = after == b"\xF0\x24"  # BEQ +36
            hits.append((cpu, "A", bail, i))
            i += 9; continue
        # LDX $0617 ... STX $0617  (within a short window)
        if prg[i:i+3] == b"\xAE\x17\x06":
            j = i + 3
            while j < min(i + 40, len(prg) - 2):
                if prg[j:j+2] == b"\x8E\x17" and prg[j+2] == 0x06:
                    after = prg[j+3:j+5]
                    bail = after in (b"\xF0",) and after[1] != 0x00
                    hits.append((base_cpu + i, "X", bail, i))
                    break
                j += 1
        i += 1
    return hits

if __name__ == "__main__":
    print("bank  addr    form  bails-on-wrap")
    for bank in list(range(7)) + [7]:
        for cpu, form, bail, _ in scan_bank(bank):
            print(f"  {bank}   ${cpu:04X}   {form}      {'YES' if bail else 'no'}")
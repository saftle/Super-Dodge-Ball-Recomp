#!/usr/bin/env python3
"""make_canary.py — build a minimal NROM-128 ROM that speaks the blargg $6000 protocol.

The canary enables NMI, counts 10 NMI frames, then reports PASS ($6000=$00
with the $DE $B0 $61 magic). It proves the harness pipeline (recompile ->
build -> run -> verdict parse) without downloading anything:

  python3 tools/blargg/selftest/make_canary.py
  python3 tools/blargg/run_blargg.py --rom tools/blargg/selftest/canary.nes --name canary

Expected: PASS in ~10 frames, 0 dispatch misses.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# Hand-assembled 6502. RESET at $C000, NMI handler at $C030, IRQ stub $C033.
CODE = [
    0x78,                   # SEI
    0xD8,                   # CLD
    0xA2, 0xFF,             # LDX #$FF
    0x9A,                   # TXS
    0xA9, 0xDE,             # LDA #$DE
    0x8D, 0x01, 0x60,       # STA $6001
    0xA9, 0xB0,             # LDA #$B0
    0x8D, 0x02, 0x60,       # STA $6002
    0xA9, 0x61,             # LDA #$61
    0x8D, 0x03, 0x60,       # STA $6003
    0xA9, 0x80,             # LDA #$80
    0x8D, 0x00, 0x60,       # STA $6000 (running)
    0xA9, 0x00,             # LDA #$00
    0x85, 0x00,             # STA $00 (frame counter)
    0xA9, 0x80,             # LDA #$80
    0x8D, 0x00, 0x20,       # STA $2000 (NMI enable)
    # wait10:
    0xA5, 0x00,             # LDA $00
    0xC9, 0x0A,             # CMP #10
    0xD0, 0xFA,             # BNE wait10
    0xA9, 0x00,             # LDA #$00
    0x8D, 0x00, 0x60,       # STA $6000 (PASS)
    # spin:
    0x4C, 0x2D, 0xC0,       # JMP spin
    # NMI ($C030):
    0xE6, 0x00,             # INC $00
    0x40,                   # RTI
    # IRQ ($C033):
    0x40,                   # RTI
]
assert len(CODE) == 0x34, len(CODE)

PRG = bytearray([0xEA] * 16384)  # NOP fill
PRG[0:len(CODE)] = bytes(CODE)
# Vectors at $FFFA (last 6 bytes of the 16KB bank).
PRG[0x3FFA:0x4000] = bytes([0x30, 0xC0, 0x00, 0xC0, 0x33, 0xC0])

hdr = bytearray(16)
hdr[0:4] = b"NES\x1a"
hdr[4] = 1   # 1 x 16KB PRG (NROM-128)
hdr[5] = 1   # 1 x 8KB CHR
hdr[6] = 0x00  # mapper 0 low nibble, horizontal mirroring, no trainer/battery
hdr[7] = 0x00  # mapper 0 high (iNES, not 2.0)

out = os.path.join(HERE, "canary.nes")
with open(out, "wb") as f:
    f.write(hdr)
    f.write(PRG)
    f.write(bytes(8192))  # blank CHR ROM
print(f"wrote {out} ({16 + 16384 + 8192} bytes)")

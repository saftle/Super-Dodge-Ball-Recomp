#!/usr/bin/env python3
"""disbank.py — tools/dis65.py with an explicit PRG bank.

MMC1 mode 3: $8000-$BFFF is switchable (PRG banks 0-6), $C000-$FFFF is fixed
(PRG bank 7).  Pass --bank to disambiguate.
"""
import subprocess
import sys

ROM = "roms/Super Dodge Ball (USA).nes"

def cpu_to_off(bank, addr):
    base = 0xC000 if bank == 7 else 0x8000
    if not (base <= addr < base + 0x4000):
        raise SystemExit(f"${addr:04X} is not in bank {bank}'s window")
    return 16 + bank * 16384 + (addr - base)

def dis(addr, count, bank):
    off = cpu_to_off(bank, addr)
    out = subprocess.run([sys.executable, "tools/dis65.py", ROM, f"{addr:04X}",
                          str(count), str(off)], capture_output=True, text=True)
    return out.stdout

if __name__ == "__main__":
    bank = int(sys.argv[1], 0)
    addr = int(sys.argv[2].replace("$", ""), 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    print(dis(addr, count, bank), end="")
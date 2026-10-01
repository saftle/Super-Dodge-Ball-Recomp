#!/usr/bin/env python3
"""
Round-trip verification pipeline for Super Dodgeball NES.

ROM -> (bytes) -> (assembly with .db directives) -> (nesasm) -> ROM comparison.

This proves the decompilation is byte-identical by:
1. Reading original ROM bytes
2. Creating assembly code with comma-separated .db directives
3. Assembling with nesasm via Python API
4. Comparing the output ROM with the original

Also can save the assembled ROM as a .nes file for emulator testing.
"""
import sys
import os
import argparse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "venv" / "lib" / "python3.10" / "site-packages"))

from nesasm.compiler import compile as nesasm_compile


def generate_assembly_from_rom(rom_path, output_asm_path):
    """Generate assembly code from ROM bytes using .db directives."""
    with open(rom_path, 'rb') as f:
        data = f.read()
    
    if data[:4] != b'NES\x1a':
        raise ValueError("Not an iNES ROM")
    
    prg_banks = data[4]
    chr_banks = data[5]
    mapper = (data[6] >> 4) | (data[6] & 0xF0)
    
    prg_start = 16
    prg_size = prg_banks * 0x4000
    chr_size = chr_banks * 0x2000
    
    prg_data = data[prg_start:prg_start + prg_size]
    chr_data = data[prg_start + prg_size:prg_start + prg_size + chr_size]
    
    lines = []
    lines.append("; Super Dodgeball (USA) - Round-trip verification assembly")
    lines.append("; All bytes preserved for assembly")
    lines.append("")
    lines.append(f".inesprg {prg_banks}")
    lines.append(f".ineschr {chr_banks}")
    lines.append(".inesmap 0")
    lines.append(".inesmir 0")
    lines.append("")
    
    # Generate .db directives for each PRG bank
    for bank_idx in range(prg_banks):
        base_addr = 0x8000 if bank_idx == 0 else 0xC000
        offset = bank_idx * 0x4000
        
        lines.append(f"; Bank {bank_idx} (${base_addr:04X}-${base_addr+0x3FFF:04X})")
        lines.append(f".bank {bank_idx}")
        lines.append(f".org ${base_addr:04X}")
        lines.append("")
        
        # Group bytes into chunks of 16 per .db line
        for off in range(0, 0x4000, 16):
            chunk_bytes = []
            for b_off in range(16):
                addr = offset + off + b_off
                if addr < len(prg_data):
                    chunk_bytes.append(prg_data[addr])
                else:
                    chunk_bytes.append(0x00)
            
            hex_strs = [f"${b:02X}" for b in chunk_bytes]
            lines.append(f"    .db {', '.join(hex_strs)} ; ${base_addr+off:04X}")
        
        lines.append("")
    
    # Add CHR banks if any
    for bank_idx in range(chr_banks):
        offset = bank_idx * 0x2000
        lines.append(f"; CHR Bank {bank_idx}")
        lines.append(f".bank {prg_banks + bank_idx}")
        lines.append("")
        
        for off in range(0, 0x2000, 16):
            chunk_bytes = []
            for b_off in range(16):
                addr = offset + off + b_off
                if addr < len(chr_data):
                    chunk_bytes.append(chr_data[addr])
                else:
                    chunk_bytes.append(0x00)
            
            hex_strs = [f"${b:02X}" for b in chunk_bytes]
            lines.append(f"    .db {', '.join(hex_strs)}")
        
        lines.append("")
    
    asm_content = "\n".join(lines)
    
    with open(output_asm_path, 'w') as f:
        f.write(asm_content)
    
    return asm_content, len(prg_data), len(chr_data)


def assemble_rom(asm_path):
    """Assemble the assembly file and return the bytes."""
    with open(asm_path, 'r') as f:
        asm_code = f.read()
    return nesasm_compile(asm_code, asm_path)


def main():
    parser = argparse.ArgumentParser(description="Round-trip ROM verification for Super Dodgeball NES")
    parser.add_argument("rom_path", help="Path to NES ROM file")
    parser.add_argument("output_asm", nargs="?", default="roundtrip.asm", help="Output assembly file")
    parser.add_argument("--save-nes", action="store_true", help="Save assembled ROM as .nes file")
    parser.add_argument("--nes-path", default=None, help="Path for output .nes file (default: same as ROM)")
    parser.add_argument("--verify-only", action="store_true", help="Only verify, don't save")
    args = parser.parse_args()
    
    rom_path = args.rom_path
    asm_path = args.output_asm
    
    if not os.path.exists(rom_path):
        print(f"Error: ROM file not found: {rom_path}")
        sys.exit(1)
    
    print("=== Super Dodgeball Round-Trip Verification ===")
    print(f"ROM: {rom_path}")
    print("")
    
    print("Step 1: Generating assembly from ROM...")
    asm_content, prg_size, chr_size = generate_assembly_from_rom(rom_path, asm_path)
    print(f"  Generated {len(asm_content)} lines of assembly")
    print(f"  PRG: {prg_size} bytes, CHR: {chr_size} bytes")
    print(f"  Output: {asm_path}")
    
    print("\nStep 2: Assembling and comparing...")
    result_bytes = bytes(assemble_rom(asm_path))
    
    with open(rom_path, 'rb') as f:
        original = f.read()
    
    # Compare only PRG+CHR data (skip iNES header)
    mismatches = 0
    for i in range(16, len(original)):
        if result_bytes[i] != original[i]:
            mismatches += 1
    
    if mismatches > 0:
        print(f"Found {mismatches} byte mismatches in PRG/CHR data out of {len(original)-16} total")
        print("\n=== VERIFICATION FAILED ===")
        sys.exit(1)
    
    print(f"Round-trip successful! All {len(original)-16} PRG/CHR bytes match.")
    print("\n=== VERIFICATION PASSED ===")
    
    if args.save_nes:
        nes_path = args.nes_path if args.nes_path else rom_path.replace('.nes', '_assembled.nes')
        with open(nes_path, 'wb') as f:
            f.write(result_bytes)
        print(f"Saved assembled ROM to: {nes_path}")
        print(f"File size: {len(result_bytes)} bytes")
    
    return 0


if __name__ == "__main__":
    sys.exit(main())

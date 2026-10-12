import sys
rom = open("roms/Super Dodge Ball (USA).nes","rb").read()
prg = rom[16:16+128*1024]
targets = [int(x,16) for x in sys.argv[1:]] if len(sys.argv)>1 else [0x0617]
abs_ops = {0x8D:"STA",0x9D:"STA,X",0x99:"STA,Y",0xAE:"LDX",0xBE:"LDX,Y",0xAD:"LDA",
           0xBD:"LDA,X",0xB9:"LDA,Y",0x8E:"STX",0x9C:"STY",0xEE:"INC",0xCE:"DEC",
           0xE6:"INC",0xC6:"DEC",0xEC:"CPX",0xCC:"CPY",0x4C:"JMP",0x2C:"BIT",0x0C:"NOP"}
for t in targets:
    print(f"===== refs to ${t:04X} =====")
    rows=[]
    for bank in range(8):
        base=bank*16384
        for i in range(16384-2):
            op=prg[base+i]
            if op not in abs_ops: continue
            a=(prg[base+i+2]<<8)|prg[base+i+1]
            if a!=t: continue
            cpu = 0xC000+i if bank==7 else 0x8000+i
            rows.append((cpu,bank,abs_ops[op]))
    rows.sort()
    for cpu,bank,nm in rows:
        print(f"  ${cpu:04X} (b{bank})  {nm}")

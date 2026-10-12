rom = open("roms/Super Dodge Ball (USA).nes","rb").read()
prg = rom[16:16+128*1024]
# MMC1 mode 3: $8000-$BFFF = switchable (banks 0-6), $C000-$FFFF = fixed (bank 7)
def cpu_of(bank, i):
    return 0xC000 + i if bank == 7 else 0x8000 + i
def off(bank, cpu):
    return 16 + bank*16384 + (cpu - (0xC000 if bank==7 else 0x8000))
store = {0x8D:"STA",0x9D:"STA,X",0x99:"STA,Y",0x99:"STA,Y"}
for a_lo,a_hi,label in ((0x0200,0x02FF,"$02xx (OAM page)"),(0x0300,0x03FF,"$03xx")):
    print(f"===== {label} =====")
    out=[]
    for bank in range(8):
        base=bank*16384
        for i in range(16384-2):
            op=prg[base+i]
            nm=None
            if op==0x8D: nm="STA"
            elif op==0x9D: nm="STA,X"
            elif op==0x99: nm="STA,Y"
            elif op==0xAE: nm="LDX"
            elif op==0xBE: nm="LDX,Y"
            elif op==0xAD: nm="LDA"
            elif op==0xBD: nm="LDA,X"
            elif op==0xB9: nm="LDA,Y"
            elif op==0xBD: nm="LDA,X"
            elif op==0x8E: nm="STX"
            elif op==0x9C: nm="STY"
            if nm is None: continue
            a=(prg[base+i+2]<<8)|prg[base+i+1]
            if a_lo<=a<=a_hi:
                out.append((cpu_of(bank,i),bank,nm,a))
    out.sort()
    for cpu,bank,nm,a in out:
        print(f"  ${cpu:04X} (b{bank})  {nm:5s} ${a:04X}")

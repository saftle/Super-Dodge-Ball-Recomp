-- oracle_dump.lua — MesenCE --testrunner oracle for Super Dodge Ball (USA), MMC1
-- Run: xvfb-run -a ./Mesen --testrunner "rom.nes" oracle_dump.lua --timeout=30
-- Env: OUT_DIR (default "."), DUMP_FRAME (default 11), MAX_FRAMES (default 13)
-- See docs/research.md §16 for API notes and limits.
local OUT = os.getenv("OUT_DIR") or "."
local DUMP_FRAME = tonumber(os.getenv("DUMP_FRAME") or "11")
local MAX_FRAMES = tonumber(os.getenv("MAX_FRAMES") or "13")

local frame = 0
local nmiCount = 0
local nmiLog = {}
local dumped = false

local function rb(addr, mt) return emu.read(addr, mt, false) end

local function dumpBytes(path, mt, base, len)
  local f = assert(io.open(path, "wb"))
  for i = 0, len - 1 do f:write(string.char(rb(base + i, mt))) end
  f:close()
end

-- per-NMI RAM flags + CPU regs. nmi event fires at the NMI vector.
emu.addEventCallback(function(cpu)
  nmiCount = nmiCount + 1
  local regs = emu.getCpuState(emu.cpuType.nes)
  local st = emu.getState()
  nmiLog[#nmiLog + 1] = string.format(
    "nmi=%d frame=%d pc=$%04X a=$%02X x=$%02X y=$%02X sp=$%02X status=$%02X " ..
    "r0100=$%02X r0106=$%02X r0107=$%02X r0108=$%02X r0168=$%02X " ..
    "scrollX=%s vramAddr=%s tmpAddr=%s writeToggle=%s",
    nmiCount, frame, regs.pc or 0, regs.a or 0, regs.x or 0, regs.y or 0,
    regs.sp or 0, regs.status or regs.ps or 0,
    rb(0x0100, emu.memType.nesInternalRam), rb(0x0106, emu.memType.nesInternalRam),
    rb(0x0107, emu.memType.nesInternalRam), rb(0x0108, emu.memType.nesInternalRam),
    rb(0x0168, emu.memType.nesInternalRam),
    tostring(st.ScrollX), tostring(st.VideoRamAddr),
    tostring(st.TmpVideoRamAddr), tostring(st.WriteToggle))
end, emu.eventType.nmi)

-- press Start on frames 12-13 (title -> game); no effect on frame-11 dump
emu.addEventCallback(function()
  if frame == 11 or frame == 12 then
    emu.setInput({start=true}, 0, 0)
  else
    emu.setInput({start=false}, 0, 0)
  end
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  if frame == DUMP_FRAME and not dumped then
    dumped = true
    -- PPU state as the game sees it (mapped, mirroring applied)
    dumpBytes(OUT.."/ppu_0000_1FFF_chr.bin",  emu.memType.nesPpuMemory, 0x0000, 0x2000)
    dumpBytes(OUT.."/ppu_2000_2FFF_nt.bin",   emu.memType.nesPpuMemory, 0x2000, 0x1000)
    dumpBytes(OUT.."/palette_32.bin",         emu.memType.nesPaletteRam, 0x0000, 0x0020)
    dumpBytes(OUT.."/ciram_2k.bin",           emu.memType.nesNametableRam, 0x0000, 0x0800)
    dumpBytes(OUT.."/oam_256.bin",            emu.memType.nesSpriteRam, 0x0000, 0x0100)
    dumpBytes(OUT.."/ram_2k.bin",             emu.memType.nesInternalRam, 0x0000, 0x0800)
    local f = assert(io.open(OUT.."/meta.txt", "w"))
    f:write(string.format("dumpFrame=%d nmiCount=%d\n", frame, nmiCount))
    local c0 = emu.convertAddress(0x0000, emu.memType.nesChrRom, emu.cpuType.nes)
    local c1 = emu.convertAddress(0x1000, emu.memType.nesChrRom, emu.cpuType.nes)
    f:write(string.format("chrLiveOff_pp0000=%s chrLiveOff_pp1000=%s\n",
      c0 and c0.address or "nil", c1 and c1.address or "nil"))
    local st = emu.getState()
    for _, k in ipairs({"VideoRamAddr","TmpVideoRamAddr","ScrollX","ScrollY","WriteToggle",
                        "Scanline","Cycle","frameCount","masterClock"}) do
      f:write(string.format("%s=%s\n", k, tostring(st[k])))
    end
    for k, v in pairs(st) do
      local kl = tostring(k):lower()
      if kl:find("bank") or kl:find("chr") or kl:find("prg") or kl:find("mmc1")
         or kl:find("mapper") or kl:find("mirror") then
        f:write(string.format("mapperkey %s=%s\n", tostring(k), tostring(v)))
      end
    end
    local r = emu.getCpuState(emu.cpuType.nes)
    f:write(string.format("cpu pc=$%04X a=$%02X x=$%02X y=$%02X sp=$%02X\n",
      r.pc or 0, r.a or 0, r.x or 0, r.y or 0, r.sp or 0))
    f:close()
    emu.takeScreenshot(OUT.."/frame_dump.png")
  end
  if frame >= MAX_FRAMES then
    local f = assert(io.open(OUT.."/nmi_log.txt", "w"))
    for _, line in ipairs(nmiLog) do f:write(line, "\n") end
    f:close()
    emu.stop(0)
  end
end, emu.eventType.endFrame)

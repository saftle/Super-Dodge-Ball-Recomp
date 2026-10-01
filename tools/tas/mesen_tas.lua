-- mesen_tas.lua — MesenCE --testrunner TAS oracle: shared-input playback + full capture.
--
-- Env:
--   TAS_LUA    path to lua file returning array of per-frame button masks
--              (recomp bits: A=0x80 B=0x40 SELECT=0x20 START=0x10 UP=0x08 DOWN=0x04 LEFT=0x02 RIGHT=0x01)
--   OUT_DIR    output dir (default ".")
--   MAX_FRAMES stop after this many endFrame events (default = #TAS)
--   SHOT_STRIDE write screenshot+bins every N frames (default 1)
--
-- Run (via run_tas.py; manual form):
--   OUT_DIR=... TAS_LUA=... MAX_FRAMES=400 HOME=<mesen-home> \
--     xvfb-run -a ./external/mesence/Mesen --testRunner "rom.nes" tools/tas/mesen_tas.lua
--
-- Input timing: setInput (all-8 table) in inputPolled only. Frame counter
-- advances in endFrame; TAS index = frame (frame 0 = first post-power frame).
local OUT = os.getenv("OUT_DIR") or "."
local TAS_PATH = os.getenv("TAS_LUA") or ""
local MAX_FRAMES = tonumber(os.getenv("MAX_FRAMES") or "0")
local SHOT_STRIDE = tonumber(os.getenv("SHOT_STRIDE") or "1")
assert(TAS_PATH ~= "", "mesen_tas: set TAS_LUA=<frames.lua>")
local ok, TAS_or_err = pcall(dofile, TAS_PATH)
do
  local mf = io.open(OUT .. "/tas_load_marker.txt", "w")
  if mf then
    if ok then mf:write(string.format("loaded %d frames\n", #TAS_or_err))
    else mf:write("LOAD FAILED: " .. tostring(TAS_or_err) .. "\n") end
    mf:close()
  end
end
assert(ok, "mesen_tas: dofile(TAS_LUA) failed: " .. tostring(TAS_or_err))
local TAS = TAS_or_err
if MAX_FRAMES == 0 then MAX_FRAMES = #TAS end

local frame = 0
local nmiCount = 0
local nmiLog = {}

local function rb(addr, mt) return emu.read(addr, mt, false) end

local function dumpBytes(path, mt, base, len)
  local f = assert(io.open(path, "wb"))
  for i = 0, len - 1 do f:write(string.char(rb(base + i, mt))) end
  f:close()
end

local function maskBits(m)
  return { a = (m & 0x80) ~= 0, b = (m & 0x40) ~= 0,
    select = (m & 0x20) ~= 0, start = (m & 0x10) ~= 0,
    up = (m & 0x08) ~= 0, down = (m & 0x04) ~= 0,
    left = (m & 0x02) ~= 0, right = (m & 0x01) ~= 0 }
end

-- per-NMI flags (cheap): regs + handshake bytes
emu.addEventCallback(function(cpu)
  nmiCount = nmiCount + 1
  local regs = emu.getCpuState(emu.cpuType.nes)
  nmiLog[#nmiLog + 1] = string.format(
    "nmi=%d frame=%d pc=$%04X a=$%02X x=$%02X y=$%02X sp=$%02X " ..
    "r0100=$%02X r0106=$%02X r0107=$%02X r0108=$%02X r0168=$%02X",
    nmiCount, frame, regs.pc or 0, regs.a or 0, regs.x or 0, regs.y or 0,
    regs.sp or 0,
    rb(0x0100, emu.memType.nesInternalRam), rb(0x0106, emu.memType.nesInternalRam),
    rb(0x0107, emu.memType.nesInternalRam), rb(0x0108, emu.memType.nesInternalRam),
    rb(0x0168, emu.memType.nesInternalRam))
end, emu.eventType.nmi)

-- TAS input (sync-safe point)
emu.addEventCallback(function()
  local m = TAS[frame + 1] or 0
  emu.setInput(maskBits(m), 0, 0)
end, emu.eventType.inputPolled)

local stateLog = assert(io.open(OUT .. "/mesen_state.jsonl", "w"))

local function js(v)
  if v == nil then return "null" end
  if type(v) == "number" then return tostring(v) end
  return string.format("%q", tostring(v))
end

emu.addEventCallback(function()
  local idx = frame
  -- cheap per-frame state line (always)
  local regs = emu.getCpuState(emu.cpuType.nes)
  local st = emu.getState()
  local m = TAS[idx + 1] or 0
  stateLog:write(string.format(
    '{"f":%d,"nmi":%d,"pc":"0x%04X","a":"0x%02X","x":"0x%02X","y":"0x%02X","sp":"0x%02X",' ..
    '"r0100":"0x%02X","r0106":"0x%02X","r0107":"0x%02X","r0168":"0x%02X",' ..
    '"scrollX":%s,"scrollY":%s,"vram":%s,"tmp":%s,"wt":%s,"cyc":%s,"clk":%s,"input":%d}\n',
    idx, nmiCount, regs.pc or 0, regs.a or 0, regs.x or 0, regs.y or 0, regs.sp or 0,
    rb(0x0100, emu.memType.nesInternalRam), rb(0x0106, emu.memType.nesInternalRam),
    rb(0x0107, emu.memType.nesInternalRam), rb(0x0168, emu.memType.nesInternalRam),
    js(st.ScrollX), js(st.ScrollY), js(st.VideoRamAddr),
    js(st.TmpVideoRamAddr), js(st.WriteToggle),
    js(st.Cycle or st.cycle), js(emu.getMasterClock and emu.getMasterClock() or 0), m))
  -- heavy dumps at stride (PPM chosen deliberately: raw P6, no encoder needed)
  if (idx % SHOT_STRIDE) == 0 then
    local tag = string.format("%s/mesen_%05d", OUT, idx)
    local size = emu.getScreenSize()
    local buffer = emu.getScreenBuffer()
    if size and buffer then
      local f = io.open(tag .. ".ppm", "wb")
      if f then
        f:write("P6\n", tostring(size.width), " ", tostring(size.height), "\n255\n")
        for i = 1, #buffer do
          local color = buffer[i]
          f:write(string.char((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF))
        end
        f:close()
      end
    end
    dumpBytes(tag .. ".ram.bin",  emu.memType.nesInternalRam, 0x0000, 0x0800)
    dumpBytes(tag .. ".chr.bin",  emu.memType.nesPpuMemory, 0x0000, 0x2000)
    dumpBytes(tag .. ".nt.bin",   emu.memType.nesPpuMemory, 0x2000, 0x1000)
    dumpBytes(tag .. ".pal.bin",  emu.memType.nesPaletteRam, 0x0000, 0x0020)
    dumpBytes(tag .. ".oam.bin",  emu.memType.nesSpriteRam, 0x0000, 0x0100)
    dumpBytes(tag .. ".ciram.bin", emu.memType.nesNametableRam, 0x0000, 0x0800)
    -- mapper/CHR-window identity (key names vary by build — enumerate)
    local c0 = emu.convertAddress(0x0000, emu.memType.nesChrRom, emu.cpuType.nes)
    local c1 = emu.convertAddress(0x1000, emu.memType.nesChrRom, emu.cpuType.nes)
    local mf = io.open(tag .. ".meta.txt", "w")
    if mf then
      mf:write(string.format("frame=%d nmi=%d input=%d\n", idx, nmiCount, m))
      mf:write(string.format("chrLive_pp0000=%s chrLive_pp1000=%s\n",
        c0 and c0.address or "nil", c1 and c1.address or "nil"))
      mf:write(string.format("cpu pc=$%04X a=$%02X x=$%02X y=$%02X sp=$%02X\n",
        regs.pc or 0, regs.a or 0, regs.x or 0, regs.y or 0, regs.sp or 0))
      for k, v in pairs(st) do
        local kl = tostring(k):lower()
        if kl:find("bank") or kl:find("chr") or kl:find("prg") or kl:find("mmc1")
           or kl:find("mapper") or kl:find("mirror") then
          mf:write(string.format("mapperkey %s=%s\n", tostring(k), tostring(v)))
        end
      end
      mf:close()
    end
  end
  frame = frame + 1
  if frame >= MAX_FRAMES then
    stateLog:close()
    local f = assert(io.open(OUT .. "/mesen_nmi_log.txt", "w"))
    for _, line in ipairs(nmiLog) do f:write(line, "\n") end
    f:close()
    emu.stop(0)
  end
end, emu.eventType.endFrame)
print("mesen_tas ready: " .. #TAS .. " input frames, max=" .. MAX_FRAMES)

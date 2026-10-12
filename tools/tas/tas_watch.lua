-- tas_watch.lua — TAS-driven who-writes probe (MesenCE --testrunner).
--
-- Env:
--   TAS_LUA  path to lua returning per-frame button masks (recomp bits) (required)
--   WATCH    comma-separated hex internal-RAM addrs, e.g. "0100,0106,0107" (required)
--   MAXF     stop after this many endFrame events (default 60)
--   OUT_DIR  output dir (default ".")
--   NMI_LOG  1 = also log $0100/$0106/SP at every NMI (default 0)
--
-- Every write to a watched addr is appended with frame, value, writing PC
-- and SP. Uncapped (streams to disk) — safe for hot staging bytes.
--
-- Run:
--   TAS_LUA=... WATCH=0106,0107 MAXF=550 OUT_DIR=... HOME=<mesen-home> \
--     xvfb-run -a ./external/mesence/Mesen --testRunner "rom.nes" tools/tas/tas_watch.lua
local OUT = os.getenv("OUT_DIR") or "."
local TAS_PATH = os.getenv("TAS_LUA") or ""
local MAXF = tonumber(os.getenv("MAXF") or "60")
local NMI_LOG = os.getenv("NMI_LOG") == "1"
local PRGLOG = os.getenv("PRGLOG") == "1"
assert(TAS_PATH ~= "", "tas_watch: set TAS_LUA=<frames.lua>")
local ok, TAS_or_err = pcall(dofile, TAS_PATH)
assert(ok, "tas_watch: dofile(TAS_LUA) failed: " .. tostring(TAS_or_err))
local TAS = TAS_or_err

local addrs = {}
for a in string.gmatch(os.getenv("WATCH") or "", "([^,]+)") do
  local n = tonumber(a, 16)
  if n then addrs[#addrs + 1] = n end
end
assert(#addrs > 0, "tas_watch: set WATCH=addr[,addr...] (hex, internal RAM)")

local log = assert(io.open(OUT .. "/tas_watch.txt", "w"))
local frame = 0

local function maskBits(m)
  return { a = (m & 0x80) ~= 0, b = (m & 0x40) ~= 0,
    select = (m & 0x20) ~= 0, start = (m & 0x10) ~= 0,
    up = (m & 0x08) ~= 0, down = (m & 0x04) ~= 0,
    left = (m & 0x02) ~= 0, right = (m & 0x01) ~= 0 }
end

for _, a in ipairs(addrs) do
  local aa = a
  emu.addMemoryCallback(function(addr, value, cpu)
    local regs = emu.getCpuState(emu.cpuType.nes)
    local extra = ""
    if PRGLOG then
      local okc, conv = pcall(emu.convertAddress, regs.pc or 0,
        emu.memType.nesPrgRom, emu.cpuType.nes)
      if okc and conv and conv.address then
        extra = string.format(" prg=$%X", conv.address)
      end
    end
    log:write(string.format("f=%d a=$%04X val=$%02X pc=$%04X sp=$%02X%s\n",
      frame, aa, value or 0, regs.pc or 0, regs.sp or 0, extra))
  end, emu.callbackType.write, aa, aa, emu.cpuType.nes, emu.memType.nesInternalRam)
end

if NMI_LOG then
  local function rb(x) return emu.read(x, emu.memType.nesInternalRam) end
  emu.addEventCallback(function(cpu)
    local regs = emu.getCpuState(emu.cpuType.nes)
    log:write(string.format("nmi f=%d pc=$%04X sp=$%02X r0100=$%02X r0106=$%02X r0107=$%02X\n",
      frame, regs.pc or 0, regs.sp or 0, rb(0x0100), rb(0x0106), rb(0x0107)))
  end, emu.eventType.nmi)
end

emu.addEventCallback(function()
  local m = TAS[frame + 1] or 0
  emu.setInput(maskBits(m), 0, 0)
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  if frame >= MAXF then
    log:close()
    emu.stop(0)
  end
end, emu.eventType.endFrame)
print("tas_watch ready: watching " .. #addrs .. " addrs over " .. MAXF .. " frames")

-- memwatch.lua — MesenCE --testrunner oracle probe: who writes RAM addrs?
--
-- Logs every write to a set of internal-RAM addresses with the writing PC,
-- SP, value and frame, plus an optional per-NMI flag line. Used to trace
-- NMI handshakes ($0100/$0106), menu state ($06B0/$06B1/$069E, ...) without
-- guessing from static disassembly.
--
-- Env:
--   OUT_DIR   output dir (default ".")
--   WATCH     comma-separated hex addrs, e.g. "0100,0106,06B0,069E" (required)
--   MAXF      stop frame (default 60)
--   START0/START1  hold Start on [START0,START1] (default 10/12, -1 disables)
--   NMI_LOG   1 = also log $0100/$0106/SP at every NMI (default 0)
--   TAPF/TAPBTN  extra single tap, e.g. TAPF=40 TAPBTN=a (default off)
--
-- Run:
--   OUT_DIR=./tmp/mw MAXF=60 WATCH=0100,0106,06B0 HOME=<mesen-home> \
--     xvfb-run -a ./external/mesence/Mesen --testrunner "rom.nes" \
--     tools/frame_gen/memwatch.lua
local OUT = os.getenv("OUT_DIR") or "."
local MAXF = tonumber(os.getenv("MAXF") or "60")
local S0 = tonumber(os.getenv("START0") or "10")
local S1 = tonumber(os.getenv("START1") or "12")
local NMI_LOG = os.getenv("NMI_LOG") == "1"
local TAPF = tonumber(os.getenv("TAPF") or "-1")
local TAPBTN = os.getenv("TAPBTN") or ""
local frame = 0
local hits = {}
local MAXH = 2000

local addrs = {}
for a in string.gmatch(os.getenv("WATCH") or "", "([^,]+)") do
  local n = tonumber(a, 16)
  if n then addrs[#addrs + 1] = n end
end
assert(#addrs > 0, "memwatch: set WATCH=addr[,addr...] (hex, internal RAM)")

local function rb(a) return emu.read(a, emu.memType.nesInternalRam) end

for _, a in ipairs(addrs) do
  local aa = a
  emu.addMemoryCallback(function(addr, value, cpu)
    if #hits >= MAXH then return end
    local regs = emu.getCpuState(emu.cpuType.nes)
    hits[#hits + 1] = string.format("f=%d a=$%04X val=$%02X pc=$%04X sp=$%02X",
      frame, aa, value or 0, regs.pc or 0, regs.sp or 0)
  end, emu.callbackType.write, aa, aa, emu.cpuType.nes, emu.memType.nesInternalRam)
end

if NMI_LOG then
  emu.addEventCallback(function(cpu)
    if #hits >= MAXH then return end
    local regs = emu.getCpuState(emu.cpuType.nes)
    hits[#hits + 1] = string.format("nmi f=%d pc=$%04X sp=$%02X r0100=$%02X r0106=$%02X",
      frame, regs.pc or 0, regs.sp or 0,
      rb(0x0100), rb(0x0106))
  end, emu.eventType.nmi)
end

emu.addEventCallback(function()
  local t = {a=false, b=false, select=false, start=false,
             up=false, down=false, left=false, right=false}
  if S0 >= 0 and frame >= S0 and frame <= S1 then t.start = true end
  if TAPF >= 0 and frame >= TAPF and frame <= TAPF + 2 and TAPBTN ~= "" then
    t[TAPBTN] = true
  end
  emu.setInput(t, 0, 0)
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  if frame >= MAXF then
    local f = assert(io.open(OUT .. "/memwatch.txt", "w"))
    for _, line in ipairs(hits) do f:write(line, "\n") end
    f:close()
    emu.stop(0)
  end
end, emu.eventType.endFrame)

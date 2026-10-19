-- zp_watch.lua — MesenCE `--testRunner` probe.
--
-- While the game is writing to the PPU, dump the CPU program counter and a
-- configurable list of zero-page variables. This is how you answer "which
-- routine is drawing this, and what does IT think it is drawing?"
--
-- WHY IT EXISTS
--   The `$2007` stream alone says where a write landed, not which code did it.
--   Once you have the PC you have the bank (which is the whole bank-numbering
--   trap in AGENTS.md), and once you have the routine's own variables you have
--   the answer without a single guess.
--
--   For Super Dodge Ball it is what named the screen builder: **PC `$D320`**
--   accounts for 5697 of 5959 PPU writes in a 300-frame window, including the
--   three 32-byte nametable blocks the game redraws every frame during a
--   camera pan (see `tools/frame_gen/vram_trace.lua`, and `docs/research.md`
--   section 25).
--
-- ENVIRONMENT
--   OUT       output path                        (required)
--   TAS_LUA   lua file returning per-frame button masks  (optional)
--   MAXF      stop after this many frames        (default 60)
--   TARGETF   only log this frame; 0 = every frame (default 0)
--   MODE      row  = log every write that starts a nametable row (dest%32==0)
--             run  = log only the FIRST write of each contiguous run
--             all  = log every write              (default run)
--   ZP        comma-separated zero-page addresses to dump, as $XX or hex.
--             Default is this cartridge's; override for any other question.
--
-- EXAMPLE
--   OUT=tmp/x/zp.txt TAS_LUA=tmp/x/frames.lua MAXF=450 TARGETF=441 MODE=row \
--     xvfb-run -a ./external/mesence/Mesen --testRunner ROM zp_watch.lua
--
-- OUTPUT
--   F00441 dest=2000 PC=8230 A=00 X=20 Y=00 | $8C=20 $8D=20 | ... val=F5
--
-- TRAPS
--   * `MODE=row` assumes 32-byte nametable rows. It is wrong for a game with a
--     non-standard tilemap, which is why `MODE=run` is the default.
--   * Reading CPU RAM from inside a memory callback can itself perturb timing
--     on some emulators. Log one frame, not a long run, unless you have to.

local OUT     = assert(os.getenv("OUT"), "zp_watch: set OUT=<path>")
local TAS_LUA = os.getenv("TAS_LUA") or ""
local MAXF    = tonumber(os.getenv("MAXF") or "60")
local TARGETF = tonumber(os.getenv("TARGETF") or "0")
local MODE    = (os.getenv("MODE") or "run"):lower()
local ZP_SPEC = os.getenv("ZP") or "C3,C4,43,61"

-- The default is this cartridge's, and it is a starting point rather than an
-- answer: $C3/$C4 are the camera (world scroll), $43 is an object's state byte
-- (it is what the margin's identity compare reads), $61 is the camera TARGET
-- (the game's own clamp at $A6AF is on this, and it is one-sided -- research.md
-- section 27). Set ZP= for the question at hand.
local zp = {}
for tok in ZP_SPEC:gmatch("[^,%s]+") do
  local s = tok:gsub("^%$", "")
  zp[#zp + 1] = tonumber(s, 16) or tonumber(s)
end
assert(#zp > 0, "zp_watch: ZP parsed to nothing")

local RAM = emu.memType.nesInternalRam
local function rb(a) return emu.read(a, RAM, true) end

local names = {}
for i = 1, #zp do names[i] = string.format("$%02X=%02X", zp[i], rb(zp[i])) end
local zpline = function() return table.concat(names, " ") end

local sched = {}
if TAS_LUA ~= "" then
  local ok, m = pcall(dofile, TAS_LUA)
  assert(ok and type(m) == "table",
         "zp_watch: TAS_LUA did not return a table: " .. tostring(m))
  sched = m
end

local frame = 0
local out   = assert(io.open(OUT, "w"))
local vh, vl, armed = 0, 0, false
local inrun  = false

out:write(string.format("# zp_watch mode=%s zp=%s maxf=%d targetf=%d\n",
        MODE, ZP_SPEC, MAXF, TARGETF))

emu.addMemoryCallback(function(addr, value)
  if TARGETF ~= 0 and frame ~= TARGETF then return end
  if addr == 0x2006 then
    if not armed then vh = value; armed = true
    else vl = vh * 256 + value; armed = false end
  elseif addr == 0x2007 then
    local first = not inrun
    inrun = true
    local want = (MODE == "all")
           or (MODE == "row" and vl % 32 == 0)
           or (MODE == "run" and first)
    if want then
      local st = emu.getCpuState(emu.cpuType.nes)
      local pc = st.pc or (st.registers and st.registers.PC) or 0
      out:write(string.format("F%05d dest=%04X PC=%04X A=%02X X=%02X Y=%02X | %s | val=%02X\n",
              frame, vl, pc, st.a or 0, st.x or 0, st.y or 0, zpline(), value))
    end
    vl = vl + 1
  end
end, emu.callbackType.write, 0x2000, 0x3FFF, emu.cpuType.nes, emu.memType.ppuDebug)

-- Any $2006 write ends the current run.
emu.addMemoryCallback(function(addr) if addr == 0x2006 then inrun = false end end,
  emu.callbackType.write, 0x2000, 0x3FFF, emu.cpuType.nes, emu.memType.ppuDebug)

emu.addEventCallback(function()
  local m = sched[frame] or 0
  emu.setInput({
    a = (m & 0x80) ~= 0, b = (m & 0x40) ~= 0, select = (m & 0x20) ~= 0,
    start = (m & 0x10) ~= 0, up = (m & 0x08) ~= 0, down = (m & 0x04) ~= 0,
    left = (m & 0x02) ~= 0, right = (m & 0x01) ~= 0,
  }, 0, 0)
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  inrun = false
  frame = frame + 1
  if (TARGETF ~= 0 and frame > TARGETF) or frame >= MAXF then
    out:write(string.format("DONE frames=%d\n", frame))
    out:close()
    emu.stop(0)
  end
end, emu.eventType.endFrame)

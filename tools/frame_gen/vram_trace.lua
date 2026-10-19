-- vram_trace.lua — MesenCE `--testRunner` probe.
--
-- Traces every CONTIGUOUS RUN of `$2007` writes, with the destination the
-- `$2006` latch left pointing at.
--
-- WHAT IT IS FOR
--   "Does this game rewrite the nametable while the camera moves, or does it
--    draw the screen once and leave it?"
--
--   Run it over a walk. If the only runs into `$2000`-`$23FF` happen at a
--   screen change, the screen is drawn once and the camera pans across it. If
--   new runs into the nametable appear every time the camera crosses a tile
--   boundary, the game is STREAMING the map and there is a wider world to
--   reconstruct.
--
--   For Super Dodge Ball the answer is the first one, and it is load-bearing:
--   the widescreen margin reconstructs the background from the nametable, so a
--   game that streamed its map would make that reconstruction read the wrong
--   frame's map. See `docs/research.md` section 25.
--
-- ENVIRONMENT
--   OUT       output path                    (required)
--   TAS_LUA   lua file returning a table of per-frame button masks, MSB-first
--             A=0x80 B=0x40 Select=0x20 Start=0x10 Up=0x08 Down=0x04
--             Left=0x02 Right=0x01           (optional; omit to press nothing)
--   MAXF      stop after this many frames    (default 60)
--   LOGFROM   only log writes at frame >= this (default 0)
--   MINN      drop runs shorter than this many bytes (default 1)
--   STEP      $2007 address increment        (default 1)
--
-- EXAMPLE
--   OUT=tmp/x/vt.txt TAS_LUA=tmp/x/frames.lua MAXF=6000 \
--     xvfb-run -a ./external/mesence/Mesen --testRunner ROM vram_trace.lua
--
-- OUTPUT
--   F01234 RUN dest=2000 n=1024 first=F5 last=F5
--
-- TRAPS
--   * `$2006` is a LATCH: the first write of a pair is the high byte and the
--     second is the low byte. Getting that backwards produces destinations that
--     are plausible and wrong.
--   * A diagnostic that fires more often than it means still looks like a
--     finding: a CHR probe that read a per-line-varying address was once
--     mistaken for a bank switch.
--   * Runs must be keyed on the DESTINATION, not on a counter. Two separate
--     blits to the same address are two runs; one blit with a mid-run `$2006`
--     rewrite is two runs too, and that is information.

local OUT     = assert(os.getenv("OUT"), "vram_trace: set OUT=<path>")
local TAS_LUA = os.getenv("TAS_LUA") or ""
local MAXF    = tonumber(os.getenv("MAXF") or "60")
local LOGFROM = tonumber(os.getenv("LOGFROM") or "0")
local MINN    = tonumber(os.getenv("MINN") or "1")
local STEP    = tonumber(os.getenv("STEP") or "1")

local sched = {}
if TAS_LUA ~= "" then
  local ok, m = pcall(dofile, TAS_LUA)
  assert(ok and type(m) == "table",
         "vram_trace: TAS_LUA did not return a table: " .. tostring(m))
  sched = m
end

local frame  = 0
local out    = assert(io.open(OUT, "w"))
local vh, vl, armed = 0, 0, false
local pending = nil
local writes  = 0

local function flush()
  if pending and (pending.n >= MINN or pending.addr < 0x3F00) then
    out:write(string.format("F%05d RUN dest=%04X n=%d first=%02X last=%02X\n",
            frame, pending.addr, pending.n, pending.first, pending.last))
  end
  pending = nil
end

emu.addMemoryCallback(function(addr, value)
  if frame < LOGFROM then return end
  if addr == 0x2006 then
    -- Do NOT flush the pending run here. The game writes $2006 TWICE per
    -- address setup (high byte, low byte), so flushing on the write itself
    -- breaks every run at its first byte and the log fills with n=1 lines that
    -- look like a thousand separate one-byte blits. Run continuity is decided
    -- below, by comparing the latched destination against the open run.
    if not armed then vh = value; armed = true
    else vl = vh * 256 + value; armed = false end
  elseif addr == 0x2007 then
    -- Run continuity: the next byte of the same run is at pending.addr+n.
    -- Comparing pending.addr == vl -- which reads as the obvious thing, and is
    -- what the first two versions did -- is ALWAYS false, because vl was
    -- already incremented past it at the end of the previous write. Every run
    -- came out length 1, which looks exactly like a game performing a thousand
    -- separate one-byte blits, and is why MINN silently filtered nothing.
    if pending and vl == pending.addr + pending.n then
      pending.n    = pending.n + 1
      pending.last = value
    else
      flush()
      pending = { addr = vl, n = 1, first = value, last = value }
    end
    vl = vl + STEP
    writes = writes + 1
    if writes % 20000 == 0 then out:flush() end
  end
end, emu.callbackType.write, 0x2000, 0x3FFF, emu.cpuType.nes, emu.memType.ppuDebug)

emu.addEventCallback(function()
  local m = sched[frame] or 0
  emu.setInput({
    a = (m & 0x80) ~= 0, b = (m & 0x40) ~= 0, select = (m & 0x20) ~= 0,
    start = (m & 0x10) ~= 0, up = (m & 0x08) ~= 0, down = (m & 0x04) ~= 0,
    left = (m & 0x02) ~= 0, right = (m & 0x01) ~= 0,
  }, 0, 0)
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  flush()
  frame = frame + 1
  if frame >= MAXF then
    out:write(string.format("DONE frames=%d writes=%d\n", frame, writes))
    out:close()
    emu.stop(0)
  end
end, emu.eventType.endFrame)

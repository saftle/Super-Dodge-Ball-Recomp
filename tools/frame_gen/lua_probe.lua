-- lua_probe.lua — MesenCE `--testRunner` self-test.
--
-- Dumps the Lua API's enumerations and then TRIES to register one memory
-- callback per candidate (memType, address range) pairing, reporting which
-- succeed. Run it once before writing any other probe.
--
-- WHY
--   MesenCE 2.2's Lua API names things differently from every earlier Mesen
--   build, and a wrong `memType` fails at registration rather than silently
--   misbehaving — but only if you actually try it instead of assuming. This
--   turns "which of these is right?" from a guess into an answer in one run.
--
-- MEASURED ON THIS BUILD (2026-10-19), and the reason this file must not hardcode
-- a name it has not dumped first:
--
--     callbackTypes: exec=2 read=0 write=1
--
--   There is **no `writeMemory`**. An earlier version of this prober used it, the
--   `pcall` swallowed the nil, and MesenCE defaulted the missing argument to `write`
--   — so the probe ran and produced a plausible file while registering a member
--   that does not exist. **A nil argument that happens to be accepted is worse
--   than one that errors**: nothing says the name was wrong.
--
-- WHAT IT ANSWERS
--   Dumps the MesenCE Lua API's enumerations and tries each memory-callback
--   registration, so the right memType/callbackType is ANSWERED rather than
--   guessed. This project has a page of MesenCE 2.2 API lore that is believed
--   rather than tested; this probe is the test.
--
--   Measured on this build: `emu.memType.ppuDebug` registers and fires,
--   `emu.memType.nesPpuDebug` does NOT register ("invalid memory type"), and
--   `callbackTypes` is `exec=2 read=0 write=1`.
--
-- ENVIRONMENT
--   OUT   output path (default /tmp/lua_probe.txt)
--   FRAMES  how many frames to run before stopping (default 20)
--
-- EXAMPLE
--   xvfb-run -a ./external/mesence/Mesen --testRunner ROM lua_probe.lua
--
-- WHY THIS IS A TOOL AND NOT A SNIPPET
--   The MesenCE invocation traps in AGENTS.md (the sandbox must own the binary
--   and its settings.json, xvfb-run word-splits a path containing spaces, and
--   --testRunner stdout carries no logs) mean getting a script to run at all
--   is most of the work. This one produces a file and nothing on stdout.

local OUT    = os.getenv("OUT") or "/tmp/lua_probe.txt"
local FRAMES = tonumber(os.getenv("FRAMES") or "20")
local out    = assert(io.open(OUT, "w"))

local function dumpEnum(title, tbl)
  local ks = {}
  for k, v in pairs(tbl) do ks[#ks + 1] = string.format("%s=%s", tostring(k), tostring(v)) end
  table.sort(ks)
  out:write(title .. ": " .. table.concat(ks, " ") .. "\n")
end

dumpEnum("callbackTypes", emu.callbackType)
dumpEnum("cpuTypes",     emu.cpuType)
dumpEnum("memTypes",     emu.memType)
dumpEnum("eventTypes",   emu.eventType)
out:flush()

local counts = {}
local tries = {
  -- name,                 cpu,                    mem,                   lo,     hi
  { "nes/ppuDebug",   emu.cpuType.nes, emu.memType.ppuDebug,           0x2000, 0x2010 },
  { "nes/ppu",        emu.cpuType.nes, emu.memType.ppu,                0x2000, 0x2010 },
  { "nes/nesPpuDebug",emu.cpuType.nes, emu.memType.nesPpuDebug,        0x2000, 0x2010 },
  { "nes/internal",   emu.cpuType.nes, emu.memType.nesInternalRam,     0x0000, 0x0010 },
  { "nes/nesMemory",  emu.cpuType.nes, emu.memType.nesMemory,          0x0000, 0x0010 },
}
for _, t in ipairs(tries) do
  local name = t[1]
  local ok, err = pcall(function()
    emu.addMemoryCallback(function(addr, value) counts[name] = (counts[name] or 0) + 1 end,
      emu.callbackType.write, t[4], t[5], t[2], t[3])
  end)
  if not ok then
    out:write(string.format("REGISTER FAILED %s: %s\n", name, tostring(err)))
  end
end
out:flush()

local frame = 0
emu.addEventCallback(function()
  frame = frame + 1
  if frame >= FRAMES then
    local ks = {}
    for k, v in pairs(counts) do ks[#ks + 1] = string.format("%s=%d", k, v) end
    table.sort(ks)
    out:write(string.format("frame%d callbacks fired: %s\n",
            FRAMES, #ks > 0 and table.concat(ks, " ") or "(none)"))
    out:close()
    emu.stop(0)
  end
end, emu.eventType.endFrame)

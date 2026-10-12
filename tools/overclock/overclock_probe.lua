-- Where is the game, really? Screenshot probe.
-- Takes a PNG at intervals plus a wide state trace, so we can SEE which screen
-- we are on instead of inferring it from two RAM bytes.

local max_frames = tonumber(os.getenv("MAX_FRAMES")) or 600
local shot_dir = os.getenv("SHOT_DIR") or "."
local out_path = os.getenv("OUT_FILE") or "/tmp/probe.csv"
local every = tonumber(os.getenv("SHOT_EVERY")) or 100

local RAM = emu.memType.nesInternalRam
local held = { start=false, a=false, b=false, select=false,
               up=false, down=false, left=false, right=false }
local frame = 0
local prevCycles = emu.getCpuCycleCount()
local f = assert(io.open(out_path, "w"))

-- Menu indices per research.md section 13: $06B1 = mode, $06EA = difficulty.
-- Plus the dispatch state ($0100/$0106) and the object/display machinery.
local function state()
  local h = {}
  for _, a in ipairs{0x0100,0x0106,0x0107,0x06B1,0x06EA,0x069E,0x067A,0x067B,0x0100} do
    h[#h+1] = emu.read(a, RAM, false)
  end
  -- Fingerprint the gameplay object block + OAM so motion is measurable.
  local fp = 0
  for i = 0x0600, 0x06FF do fp = (fp * 31 + emu.read(i, RAM, false)) % 0xFFFFFFFF end
  local oam = 0
  for i = 0, 255 do oam = (oam * 31 + emu.read(i, emu.memType.nesSpriteRam, false)) % 0xFFFFFFFF end
  return table.concat(h, ",") .. "," .. fp .. "," .. oam
end

emu.addEventCallback(function()
  -- Start at 11-12 (the pattern proven in tools/frame_gen/oracle_dump.lua),
  -- then walk the menus: MODE MENU -> skill -> team-select -> match.
  if frame == 11 or frame == 12 then
    held.start = true
  elseif frame == 60 or frame == 61 then
    held.start = true            -- confirm WORLD CUP
  elseif frame == 140 or frame == 141 then
    held.start = true            -- confirm SKILL LEVEL
  elseif frame == 900 or frame == 901 then
    held.start = true            -- confirm CHANGE POSITION? -> match
  else
    held.start = false
  end
  emu.setInput({ start=held.start, a=held.a, b=held.b, select=held.select,
                 up=held.up, down=held.down, left=held.left, right=held.right },
               0, 0)
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  frame = frame + 1
  local now = emu.getCpuCycleCount()
  f:write(string.format("%d,%d,%s\n", frame, now - prevCycles, state()))
  prevCycles = now
  if false then
    -- Same PPM path as tools/frame_gen/export_frames.lua; emu.takeScreenshot()
    -- is a no-op under --testRunner.
    local buf = emu.getScreenBuffer()
    local sz = emu.getScreenSize()
    local g = assert(io.open(string.format("%s/shot_%04d.ppm", shot_dir, frame), "wb"))
    -- getScreenSize() returns NAMED fields .width/.height, not [1]/[2].
    -- getScreenBuffer() returns only the scanlines rendered since the last
    -- callback (measured: 80 of 240), NOT a whole frame. Pad the remainder so
    -- the PPM is well-formed; a short write makes the file unviewable.
    local w, h = sz.width, sz.height
    g:write("P6\n", tostring(w), " ", tostring(h), "\n255\n")
    local written = 0
    for i = 1, #buf do
      local c = buf[i]
      g:write(string.char((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF))
      written = written + 1
    end
    for _ = written + 1, w * h do g:write(string.char(0, 0, 0)) end
    g:close()
  end
  if frame >= max_frames then
    f:close()
    emu.stop(0)
  end
end, emu.eventType.endFrame)
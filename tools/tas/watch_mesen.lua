-- watch_mesen.lua — visual TAS playback on MesenCE (GUI mode, NOT --testRunner).
--
-- Env:
--   TAS_LUA    path to lua file returning array of per-frame button masks
--              (A=0x80 B=0x40 SELECT=0x20 START=0x10 UP=0x08 DOWN=0x04 LEFT=0x02 RIGHT=0x01)
--   MAX_FRAMES stop overriding input after this many endFrame events
--              (default = #TAS). The game keeps running: after the TAS ends
--              the script stops calling setInput so you can take over.
--   SHOT_AT    optional frame index to emu.takeScreenshot() (proves rendering)
--   SHOT_PATH  where to write that screenshot (default OUT_DIR/mesen_watch.png)
--   OUT_DIR    output dir (default ".")
--
-- Run (via watch_mesen.py; manual form):
--   TAS_LUA=... MAX_FRAMES=... HOME=<mesen-home> ./external/mesence/Mesen "rom.nes" tools/tas/watch_mesen.lua
--
-- Input timing: setInput (all-8 table) in inputPolled only — the one safe
-- point for scripted input. Frame counter advances in endFrame;
-- TAS index = frame (frame 0 = first post-power frame).
local OUT = os.getenv("OUT_DIR") or "."
local TAS_PATH = os.getenv("TAS_LUA") or ""
local MAX_FRAMES = tonumber(os.getenv("MAX_FRAMES") or "0")
local SHOT_AT = tonumber(os.getenv("SHOT_AT") or "-1")
local SHOT_PATH = os.getenv("SHOT_PATH") or (OUT .. "/mesen_watch.png")
assert(TAS_PATH ~= "", "watch_mesen: set TAS_LUA=<frames.lua>")
local ok, TAS_or_err = pcall(dofile, TAS_PATH)
assert(ok, "watch_mesen: dofile(TAS_LUA) failed: " .. tostring(TAS_or_err))
local TAS = TAS_or_err
if MAX_FRAMES == 0 then MAX_FRAMES = #TAS end

local frame = 0

local function maskBits(m)
  return { a = (m & 0x80) ~= 0, b = (m & 0x40) ~= 0,
    select = (m & 0x20) ~= 0, start = (m & 0x10) ~= 0,
    up = (m & 0x08) ~= 0, down = (m & 0x04) ~= 0,
    left = (m & 0x02) ~= 0, right = (m & 0x01) ~= 0 }
end

emu.addEventCallback(function()
  if frame < MAX_FRAMES then
    local m = TAS[frame + 1] or 0
    emu.setInput(maskBits(m), 0, 0)
  end
  -- past MAX_FRAMES: no setInput, the player's keyboard/pad owns P1 again
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  if frame == SHOT_AT then emu.takeScreenshot(SHOT_PATH) end
  frame = frame + 1
end, emu.eventType.endFrame)
print("watch_mesen ready: " .. #TAS .. " input frames, max=" .. MAX_FRAMES)

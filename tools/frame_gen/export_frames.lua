-- MesenCE-compatible frame export script
-- MesenCE --testrunner mode runs frames automatically.
-- Register an endFrame callback to capture each frame.
--
-- Usage:
--   HOME=<config_dir> xvfb-run -a "$MESEN_BIN" --testRunner "rom.nes" "export_frames.lua" --timeout=120

local frame_count = 0
local max_frames = 10
local output_dir = "/tmp/nes_reference"

function onEndFrame(cpuType)
    frame_count = frame_count + 1
    local size = emu.getScreenSize()
    local buffer = emu.getScreenBuffer()
    if size and buffer then
        local w = size.width
        local h = size.height
        local path = string.format("%s/frame_%04d.png", output_dir, frame_count - 1)
        local f = io.open(path, "wb")
        if f then
            f:write("P6\n", tostring(w), " ", tostring(h), "\n255\n")
            for i = 1, #buffer do
                local color = buffer[i]
                f:write(string.char((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF))
            end
            f:close()
            print("Frame " .. (frame_count - 1) .. ": " .. path)
        end
    end
    if frame_count >= max_frames then
        emu.stop()
    end
end

emu.addEventCallback(onEndFrame, emu.eventType.endFrame)
print("Ready. Waiting for frames...")

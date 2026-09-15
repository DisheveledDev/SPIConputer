-- A centred line: one display op through the Screen framework.
local function text(row, message, attr)
    Screen.CenterText(row, message, attr or 0)
end

local function draw_boot()
    Screen.Clear()
    text(8, "SPI COMPUTER", 0)
    text(10, "STARTING SYSTEM", 0)
    text(12, "PLEASE WAIT", 0)
    Screen.Clean(8, 14, 31, 14)
end

local function draw_failure()
    Screen.Clear()
    text(7, "SPI COMPUTER", 0)
    text(10, "SYSTEM STARTUP FAILED", Attributes.Inverse)
    text(13, "CANNOT FIND OS IMAGE", Attributes.Inverse)
    text(16, "EXPECTED CORE/OS.PRG", 0)
    text(17, "OR CORE/OS.LUA", 0)
    text(20, "CHECK THE SD CARD", 0)
end

-- Try each OS image once after the static startup screen has been shown.
-- Launch(..., true) replaces this program on success. A failure leaves
-- this program running with the error message on screen.
local function start_os()
    local ok = Launch("core/os.prg", nil, true)
    if not ok then
        ok = Launch("core/os.lua", nil, true)
    end
    if not ok then
        draw_failure()
    end
end

function setup()
    Screen.Mode(1)
    Screen.Palette(0, 0, 0, 160)
    Screen.Palette(1, 255, 255, 255)
    Screen.Palette(2, 180, 220, 255)
    draw_boot()
    Timer.After(2000, start_os)
end

local started = false
local failed = false
local started_at = 0
local spinner = {"|", "/", "-", "\\"}

local function text(row, message, attr)
    local x = math.max(0, math.floor((40 - #message) / 2))
    for i = 1, #message do
        ScreenOut(x + i - 1, row, message:byte(i), attr or 0)
    end
end

local function draw_boot(frame)
    ScreenZOrder(0)
    ScreenClear(32)
    text(8, "SPI COMPUTER", 0)
    text(10, "STARTING SYSTEM", 0)
    text(12, "PLEASE WAIT " .. spinner[(frame % #spinner) + 1], 0)
    for x = 8, 31 do
        ScreenOut(x, 14, 32, 0x02)
    end
    for x = 8, 8 + frame * 6 do
        ScreenOut(x, 14, 32, 0x00)
    end
end

local function draw_failure()
    ScreenZOrder(0)
    ScreenClear(32)
    text(7, "SPI COMPUTER", 0)
    text(10, "SYSTEM STARTUP FAILED", 0x80)
    text(13, "CANNOT FIND OS IMAGE", 0x80)
    text(16, "EXPECTED CORE/OS.PRG", 0)
    text(17, "OR CORE/OS.LUA", 0)
    text(20, "CHECK THE SD CARD", 0)
end

function setup()
    ScreenMode(1)
    ScreenPalette(0, 0, 0, 160)
    ScreenPalette(1, 255, 255, 255)
    ScreenPalette(2, 180, 220, 255)
    started_at = TimeNow()
    draw_boot(0)
end

function tick()
    if started or failed then
        return
    end
    local elapsed = TimeNow() - started_at
    local frame = math.floor(elapsed / 150) % #spinner
    draw_boot(frame)
    if elapsed < 600 then
        return
    end
    started = true
    local ok = Launch("core/os.prg")
    if not ok then
        ok = Launch("core/os.lua")
    end
    if not ok then
        failed = true
        draw_failure()
    end
end

local spinner = {"|", "/", "-", "\\"}
local started_at = 0
local handed_over = false
local update_timer = 0

local function text(row, message, attr)
    local x = math.max(0, math.floor((40 - #message) / 2))
    for i = 1, #message do
        ScreenOut(x + i - 1, row, message:byte(i), attr or 0)
    end
end

local function draw_boot(frame, progress)
    ScreenClear(32)
    text(8, "SPI COMPUTER", 0)
    text(10, "STARTING SYSTEM", 0)
    text(12, "PLEASE WAIT " .. spinner[(frame % #spinner) + 1], 0)
    for x = 8, 31 do
        ScreenOut(x, 14, 32, 0x02)
    end
    local filled = 8 + math.floor(23 * progress)
    for x = 8, filled do
        ScreenOut(x, 14, 32, 0x00)
    end
end

local function draw_failure()
    ScreenClear(32)
    text(7, "SPI COMPUTER", 0)
    text(10, "SYSTEM STARTUP FAILED", 0x80)
    text(13, "CANNOT FIND OS IMAGE", 0x80)
    text(16, "EXPECTED CORE/OS.PRG", 0)
    text(17, "OR CORE/OS.LUA", 0)
    text(20, "CHECK THE SD CARD", 0)
end

-- Timer body: animate the spinner, then hand the machine to the shell.
-- Launch(..., true) replaces this program, so this Lua state and its
-- screen slot go away once the callback returns; a failure keeps this
-- program alive showing the message instead.
local function update()
    if handed_over then
        return
    end
    local elapsed = TimeNow() - started_at
    draw_boot(math.floor(elapsed / 150), math.min(elapsed, 600) / 600)
    if elapsed < 600 then
        return
    end
    handed_over = true
    local ok = Launch("core/os.prg", nil, true)
    if not ok then
        ok = Launch("core/os.lua", nil, true)
    end
    if not ok then
        draw_failure()
        TimerStop(update_timer)
    end
end

function setup()
    ScreenMode(1)
    ScreenPalette(0, 0, 0, 160)
    ScreenPalette(1, 255, 255, 255)
    ScreenPalette(2, 180, 220, 255)
    started_at = TimeNow()
    draw_boot(0, 0)
    update_timer = TimerCreate(update, 150)
end

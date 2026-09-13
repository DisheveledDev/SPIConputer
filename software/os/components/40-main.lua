-- Startup: C64-style colours, banner and the first READY. prompt.

local function banner()
    out("")
    out("     **** SPIComputer OS v1.0 ****")
    out("")
    local free_kb = 0
    local ok, free = pcall(fs.free)
    if ok and type(free) == "number" then
        free_kb = free
    end
    if free_kb >= 1024 then
        out(string.format(" %s   SD CARD %d MB FREE", _VERSION, free_kb // 1024))
    else
        out(string.format(" %s   SD CARD %d KB FREE", _VERSION, free_kb))
    end
    out("")
    out("READY.")
end

function setup()
    ScreenMode(1) -- 40x30 tiles, invert + colour
    ScreenPalette(0, 0x00, 0x00, 0xAA) -- background: CPC blue
    ScreenPalette(1, 0xFF, 0xFF, 0x00) -- default text: CPC yellow
    ScreenPalette(2, 0xFF, 0xFF, 0x00) -- attribute colour 1: CPC yellow
    banner()
end

-- Startup: C64-style colours, banner and the first READY. prompt.

local function banner()
    out("")
    out("     **** SPIOS - SPIComputer OS ****")
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
    ScreenPalette(0, 0x40, 0x40, 0xE0) -- background: C64 blue
    ScreenPalette(1, 0x7C, 0x70, 0xDA) -- default text: light blue
    ScreenPalette(2, 0xFF, 0xFF, 0xFF) -- attribute colour 1: white
    banner()
end

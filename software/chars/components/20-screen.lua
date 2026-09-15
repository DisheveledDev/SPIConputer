-- Drawing.

local function cell(c)
    return GRID_X + (c % 16) * 2, GRID_Y + c // 16
end

local function draw_grid()
    Screen.Box(0, 1, 39, 20, Screen.SINGLE)
    Screen.OutText(GRID_X, GRID_Y - 1, "0 1 2 3 4 5 6 7 8 9 A B C D E F", LABEL_ATTR)
    for row = 0, 15 do
        local glyphs = {}
        for col = 0, 15 do
            glyphs[#glyphs + 1] = string.char(row * 16 + col)
        end
        Screen.OutText(3, GRID_Y + row, string.format("%Xx", row), LABEL_ATTR)
        Screen.OutText(GRID_X, GRID_Y + row, table.concat(glyphs, " "))
    end
end

local function binary(n)
    local bits = {}
    for i = 7, 0, -1 do bits[#bits + 1] = (n >> i) & 1 end
    return table.concat(bits)
end

-- The info panel: numbers, name, and the glyph in every colour (one
-- write of the characters, one attribute write for their colours).
local function draw_info()
    Screen.Label(1, INFO_Y, 38, string.format("CODE %3d   HEX %02X   BIN %s", code, code, binary(code)),
                 "left", LABEL_ATTR)
    Screen.Label(1, INFO_Y + 1, 38, glyph_name(code), "left")
    local ch = string.char(code)
    local glyphs, attrs = {}, {}
    for i, colour in ipairs(COLOURS) do
        glyphs[i * 2 - 1], glyphs[i * 2] = ch, ch
        attrs[i * 2 - 1] = string.char(colour[2])
        attrs[i * 2] = string.char(colour[2] + Attributes.Inverse)
    end
    local x = 13
    Screen.OutText(x, INFO_Y + 3, table.concat(glyphs, ""))
    -- All 16 colours in one op.
    Screen.OutAttrs(x, INFO_Y + 3, table.concat(attrs, ""))
end

local function move_to(new)
    local x, y = cell(code)
    Screen.Attr(x, y, Attributes.Normal)
    code = new % 256
    x, y = cell(code)
    Screen.Attr(x, y, CURSOR_ATTR)
    draw_info()
end

local function draw_all()
    Screen.Label(0, 0, 40, " CHARACTER SET          256 ROM glyphs", "left", TITLE_ATTR)
    draw_grid()
    Screen.OutText(1, INFO_Y + 3, "colours:", LABEL_ATTR)
    Screen.OutText(13, INFO_Y + 4, "W R C P G B Y O", LABEL_ATTR)
    Screen.OutText(1, INFO_Y + 5, "each colour plain, then inverted")
    Screen.Label(0, 29, 40, " ARROWS move  RET next row  ESC quit", "left", KEYS_ATTR)
    move_to(code)
end

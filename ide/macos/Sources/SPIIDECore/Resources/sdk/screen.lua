-- SDK: Screen
-- Summary: Text-mode drawing on the base layer: text, boxes, fills, block moves.
-- Namespaces: Screen, Attributes
--
-- A framework file the IDE injects, read-only, into programs that
-- select it. Its format is a contract with the builder:
--   * everything above the first top-level `function` is the preamble
--     and is always emitted;
--   * each `function Name.Sub(...)` ... `end` block (both at column 0)
--     is emitted only when the program refers to Name.Sub, directly or
--     from another emitted block;
--   * a top-level `local function helper(...)` ... `end` block is
--     emitted when an emitted block refers to `helper`;
--   * the `--- Name.Sub(x, y [, attr])` line above a block is its
--     signature for completion and parameter help; the `--` lines that
--     follow describe it.
--
-- Conventions: coordinates are 0-based cells; region calls take the two
-- inclusive corners (x1, y1, x2, y2), unlike the raw ScreenX calls,
-- which take (x, y, w, h). Every call returns what the OS call returns
-- (true, or nil and a message).

Screen = Screen or {}
Screen.COLS = 40
Screen.ROWS = 30
Screen.SINGLE = 1      -- box style: single line
Screen.DOUBLE = 2      -- box style: double line
Screen.INVERT = 0x80   -- attribute bit: swap foreground and background

-- Attribute values by name, for the `attr` argument of every drawing
-- call (Screen and Overlay): a colour, plus Attributes.Inverse to paint
-- the cell's background in that colour instead of its text. Add them:
-- `Attributes.Red + Attributes.Inverse` is a red block behind a space.
-- The colours are the mode-1 default palette (entries 1-8); a program
-- that changes the palette with Screen.Palette recolours them.
Attributes = Attributes or {}
Attributes.Normal = 0          -- white text on the background, not inverted
Attributes.White = 0
Attributes.Red = 1
Attributes.Cyan = 2
Attributes.Purple = 3
Attributes.Green = 4
Attributes.Blue = 5
Attributes.Yellow = 6
Attributes.Orange = 7
Attributes.Inverse = 0x80      -- swap text and background (a coloured block for a space)

-- Console-style output: OutLine writes at the cursor row and moves on,
-- scrolling the screen when it reaches the bottom.
local cursor_row = 0

-- Inclusive corners to (x, y, w, h), in either corner order.
local function region(x1, y1, x2, y2)
    if x2 < x1 then x1, x2 = x2, x1 end
    if y2 < y1 then y1, y2 = y2, y1 end
    return x1, y1, x2 - x1 + 1, y2 - y1 + 1
end

--- Screen.Mode(mode)
-- Selects a screen mode: 0 (40x30 B&W), 1 (40x30 colour), 10 (320x240
-- pixels). Switching clears the screen.
function Screen.Mode(mode)
    return ScreenMode(mode)
end

--- Screen.Clear([char])
-- Fills the whole base layer with `char` (default space) and attribute 0.
function Screen.Clear(char)
    return ScreenClear(char or 32)
end

--- Screen.Out(x, y, char [, attr])
-- Writes one character code at a cell.
function Screen.Out(x, y, char, attr)
    return ScreenOut(x, y, char, attr or 0)
end

--- Screen.Attr(x, y, attr)
-- Sets one cell's attribute, keeping its character.
function Screen.Attr(x, y, attr)
    return ScreenAttr(x, y, attr)
end

--- Screen.OutText(x, y, text [, attr])
-- Writes a string from (x, y), wrapping at the right edge. One display
-- op however long the text.
function Screen.OutText(x, y, text, attr)
    return ScreenWrite(x, y, tostring(text), attr)
end

--- Screen.CenterText(y, text [, attr])
-- Writes a string centred on row `y`.
function Screen.CenterText(y, text, attr)
    text = tostring(text)
    local x = (Screen.COLS - #text) // 2
    if x < 0 then x = 0 end
    return ScreenWrite(x, y, text, attr)
end

--- Screen.RightText(y, text [, attr])
-- Writes a string ending at the right edge of row `y`.
function Screen.RightText(y, text, attr)
    text = tostring(text)
    local x = Screen.COLS - #text
    if x < 0 then x = 0 end
    return ScreenWrite(x, y, text, attr)
end

--- Screen.Printf(x, y, format, ...)
-- string.format, written at (x, y).
function Screen.Printf(x, y, format, ...)
    return ScreenWrite(x, y, string.format(format, ...))
end

--- Screen.OutLine(text [, attr])
-- Console-style output: writes `text` on the cursor row (row 0 to start
-- with), then moves the cursor to the next row, scrolling the whole
-- screen up when it runs off the bottom. Long text wraps.
function Screen.OutLine(text, attr)
    text = tostring(text)
    local rows = 1 + (#text - 1) // Screen.COLS
    if #text == 0 then rows = 1 end
    while cursor_row + rows > Screen.ROWS do
        ScreenScroll(0, 0, Screen.COLS, Screen.ROWS, 0, -1, 32, 0)
        cursor_row = cursor_row - 1
    end
    local ok, err = ScreenWrite(0, cursor_row, text, attr)
    cursor_row = cursor_row + rows
    return ok, err
end

--- Screen.Cursor(row)
-- Sets the row the next Screen.OutLine writes on. Returns the row.
function Screen.Cursor(row)
    if row then
        if row < 0 then row = 0 end
        if row >= Screen.ROWS then row = Screen.ROWS - 1 end
        cursor_row = row
    end
    return cursor_row
end

--- Screen.Home()
-- Clears the screen and puts the OutLine cursor back on row 0.
function Screen.Home()
    cursor_row = 0
    return ScreenClear(32)
end

--- Screen.Clean(x1, y1, x2, y2 [, char [, attr]])
-- Blanks the cells between two corners (inclusive): `char` defaults to
-- space and `attr` to 0.
function Screen.Clean(x1, y1, x2, y2, char, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    return ScreenFill(x, y, w, h, char or 32, attr or 0)
end

--- Screen.Fill(x1, y1, x2, y2, char [, attr])
-- Fills the cells between two corners with a character and attribute.
function Screen.Fill(x1, y1, x2, y2, char, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    return ScreenFill(x, y, w, h, char, attr or 0)
end

--- Screen.FillAttr(x1, y1, x2, y2, attr)
-- Sets the attribute of every cell between two corners; characters stay.
function Screen.FillAttr(x1, y1, x2, y2, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    return ScreenFillAttr(x, y, w, h, attr)
end

--- Screen.HLine(x1, x2, y [, char [, attr]])
-- A horizontal line of `char` (default: the box-drawing horizontal).
function Screen.HLine(x1, x2, y, char, attr)
    local x, _, w = region(x1, y, x2, y)
    return ScreenFill(x, y, w, 1, char or 196, attr or 0)
end

--- Screen.VLine(x, y1, y2 [, char [, attr]])
-- A vertical line of `char` (default: the box-drawing vertical).
function Screen.VLine(x, y1, y2, char, attr)
    local _, y, _, h = region(x, y1, x, y2)
    return ScreenFill(x, y, 1, h, char or 179, attr or 0)
end

--- Screen.Box(x1, y1, x2, y2 [, style [, attr]])
-- A frame between two corners: style Screen.SINGLE (default) or
-- Screen.DOUBLE. The inside is left alone.
function Screen.Box(x1, y1, x2, y2, style, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    return ScreenBox(x, y, w, h, style or 1, attr or 0)
end

--- Screen.Window(x1, y1, x2, y2 [, title [, style [, attr]]])
-- A blank window: the inside cleared, a frame around it and an optional
-- title on the top edge.
function Screen.Window(x1, y1, x2, y2, title, style, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    attr = attr or 0
    ScreenFill(x, y, w, h, 32, attr)
    local ok, err = ScreenBox(x, y, w, h, style or 1, attr)
    if ok and title and #tostring(title) > 0 then
        local label = " " .. tostring(title) .. " "
        local tx = x + (w - #label) // 2
        if tx < x + 1 then tx = x + 1 end
        ScreenWrite(tx, y, label:sub(1, w - 2), attr)
    end
    return ok, err
end

--- Screen.Copy(x1, y1, x2, y2, x3, y3)
-- Copies the block between two corners so its top-left lands at (x3, y3).
function Screen.Copy(x1, y1, x2, y2, x3, y3)
    local x, y, w, h = region(x1, y1, x2, y2)
    return ScreenCopy(x, y, w, h, x3, y3)
end

--- Screen.Move(x1, y1, x2, y2, x3, y3 [, char [, attr]])
-- Moves the block between two corners so its top-left lands at (x3, y3),
-- blanking what it uncovers with `char` (default space) and `attr`.
function Screen.Move(x1, y1, x2, y2, x3, y3, char, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    return ScreenMove(x, y, w, h, x3, y3, char or 32, attr or 0)
end

--- Screen.Scroll(x1, y1, x2, y2, dx, dy [, char [, attr]])
-- Shifts the contents of a region by (dx, dy) cells; the cells it
-- uncovers are filled with `char` (default space) and `attr`.
function Screen.Scroll(x1, y1, x2, y2, dx, dy, char, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    return ScreenScroll(x, y, w, h, dx, dy, char or 32, attr or 0)
end

--- Screen.ScrollUp([lines [, char [, attr]]])
-- Scrolls the whole screen up by `lines` (default 1).
function Screen.ScrollUp(lines, char, attr)
    return ScreenScroll(0, 0, Screen.COLS, Screen.ROWS, 0, -(lines or 1), char or 32, attr or 0)
end

--- Screen.ScrollDown([lines [, char [, attr]]])
-- Scrolls the whole screen down by `lines` (default 1).
function Screen.ScrollDown(lines, char, attr)
    return ScreenScroll(0, 0, Screen.COLS, Screen.ROWS, 0, lines or 1, char or 32, attr or 0)
end

--- Screen.ScrollLeft([cols [, char [, attr]]])
-- Scrolls the whole screen left by `cols` (default 1).
function Screen.ScrollLeft(cols, char, attr)
    return ScreenScroll(0, 0, Screen.COLS, Screen.ROWS, -(cols or 1), 0, char or 32, attr or 0)
end

--- Screen.ScrollRight([cols [, char [, attr]]])
-- Scrolls the whole screen right by `cols` (default 1).
function Screen.ScrollRight(cols, char, attr)
    return ScreenScroll(0, 0, Screen.COLS, Screen.ROWS, cols or 1, 0, char or 32, attr or 0)
end

--- Screen.Map(chars [, attrs])
-- Replaces the whole character map from a 1200-byte string (row by
-- row), and optionally the attribute map from a second one.
function Screen.Map(chars, attrs)
    local ok, err = ScreenWrite(0, 0, chars)
    if ok and attrs then
        ok, err = ScreenWriteAttr(0, 0, attrs)
    end
    return ok, err
end

--- Screen.Palette(index, r, g, b)
-- Sets one palette entry (0 = background, 1 = default text colour).
function Screen.Palette(index, r, g, b)
    return ScreenPalette(index, r, g, b)
end

--- Screen.PaletteSet(colours)
-- Sets palette entries from an array of {r, g, b} tables or 0xRRGGBB numbers.
function Screen.PaletteSet(colours)
    return ScreenPaletteSet(colours)
end

--- Screen.DefineTile(index, rows)
-- Redefines a character's 8x8 tile from eight row bytes (table or string).
function Screen.DefineTile(index, rows)
    return ScreenDefineTile(index, rows)
end

--- Screen.Plot(x, y, colour)
-- Sets one pixel in mode 10.
function Screen.Plot(x, y, colour)
    return ScreenPlot(x, y, colour)
end

--- Screen.LoadImage(path, x, y [, w, h])
-- Draws an image file from the card at (x, y). Reserved: the OS loader
-- is not available yet, so this returns nil and a message for now.
function Screen.LoadImage(path, x, y, w, h)
    return ScreenLoadImage(path, x, y, w, h)
end

--- Screen.OutLines(x, y, lines [, attr])
-- Writes an array of strings on consecutive rows from (x, y). Returns
-- the number of rows written (rows below the screen are dropped).
function Screen.OutLines(x, y, lines, attr)
    local n = 0
    for i, line in ipairs(lines) do
        local row = y + i - 1
        if row >= Screen.ROWS then break end
        ScreenWrite(x, row, tostring(line), attr)
        n = n + 1
    end
    return n
end

-- Word-wrap helper shared by OutWrapped and Label.
local function wrap_words(s, width)
    local lines = {}
    for paragraph in (tostring(s) .. "\n"):gmatch("([^\n]*)\n") do
        local line = ""
        for token in paragraph:gmatch("%S+") do
            local word = token -- loop variables are const in Lua 5.5
            while #word > width do
                if #line > 0 then lines[#lines + 1] = line line = "" end
                lines[#lines + 1] = word:sub(1, width)
                word = word:sub(width + 1)
            end
            if #line == 0 then
                line = word
            elseif #line + 1 + #word <= width then
                line = line .. " " .. word
            else
                lines[#lines + 1] = line
                line = word
            end
        end
        lines[#lines + 1] = line
    end
    return lines
end

--- Screen.OutWrapped(x, y, width, text [, attr])
-- Word-wraps `text` to `width` cells and writes the lines from (x, y).
-- Returns the number of rows used.
function Screen.OutWrapped(x, y, width, text, attr)
    local lines = wrap_words(text, width)
    local n = 0
    for i, line in ipairs(lines) do
        local row = y + i - 1
        if row >= Screen.ROWS then break end
        ScreenFill(x, row, width, 1, 32, attr or 0)
        ScreenWrite(x, row, line, attr)
        n = n + 1
    end
    return n
end

--- Screen.Label(x, y, width, text, align [, attr])
-- Writes `text` inside a `width`-cell field, padded and aligned "left",
-- "center" or "right" (default left); longer text is cut to the field.
function Screen.Label(x, y, width, text, align, attr)
    text = tostring(text)
    if #text > width then text = text:sub(1, width) end
    local space = width - #text
    local before = 0
    if align == "center" then before = space // 2
    elseif align == "right" then before = space end
    local padded = string.rep(" ", before) .. text .. string.rep(" ", space - before)
    return ScreenWrite(x, y, padded, attr)
end

--- Screen.Progress(x, y, width, fraction [, attr])
-- A progress bar `width` cells wide: solid blocks for `fraction` (0..1)
-- of it, light shade for the rest.
function Screen.Progress(x, y, width, fraction, attr)
    if fraction < 0 then fraction = 0 end
    if fraction > 1 then fraction = 1 end
    local filled = math.floor(width * fraction + 0.5)
    if filled > 0 then ScreenFill(x, y, filled, 1, 219, attr or 0) end
    if filled < width then ScreenFill(x + filled, y, width - filled, 1, 176, attr or 0) end
    return true
end

--- Screen.Rect(x1, y1, x2, y2, char [, attr])
-- The outline of a rectangle drawn with one character (Box draws frames
-- with the box glyphs; this is for any character, e.g. a block).
function Screen.Rect(x1, y1, x2, y2, char, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    attr = attr or 0
    ScreenFill(x, y, w, 1, char, attr)
    ScreenFill(x, y + h - 1, w, 1, char, attr)
    ScreenFill(x, y, 1, h, char, attr)
    return ScreenFill(x + w - 1, y, 1, h, char, attr)
end

--- Screen.Line(x1, y1, x2, y2, char [, attr])
-- A straight line of `char` cells between two cells (Bresenham).
function Screen.Line(x1, y1, x2, y2, char, attr)
    attr = attr or 0
    local dx, dy = math.abs(x2 - x1), -math.abs(y2 - y1)
    local sx, sy = x1 < x2 and 1 or -1, y1 < y2 and 1 or -1
    local err = dx + dy
    local x, y = x1, y1
    while true do
        if x >= 0 and x < Screen.COLS and y >= 0 and y < Screen.ROWS then
            ScreenOut(x, y, char, attr)
        end
        if x == x2 and y == y2 then break end
        local e2 = 2 * err
        if e2 >= dy then err = err + dy x = x + sx end
        if e2 <= dx then err = err + dx y = y + sy end
    end
    return true
end

--- Screen.Shade(x1, y1, x2, y2, level [, attr])
-- Fills a region with a shade: level 0 (blank), 1 (light), 2 (medium),
-- 3 (dark) or 4 (solid).
function Screen.Shade(x1, y1, x2, y2, level, attr)
    local glyphs = { [0] = 32, 176, 177, 178, 219 }
    local x, y, w, h = region(x1, y1, x2, y2)
    return ScreenFill(x, y, w, h, glyphs[math.max(0, math.min(4, math.floor(level)))], attr or 0)
end

--- Screen.PixelLine(x1, y1, x2, y2, colour)
-- Mode 10: a straight line of pixels (Bresenham) in a palette colour.
function Screen.PixelLine(x1, y1, x2, y2, colour)
    local dx, dy = math.abs(x2 - x1), -math.abs(y2 - y1)
    local sx, sy = x1 < x2 and 1 or -1, y1 < y2 and 1 or -1
    local err = dx + dy
    local x, y = x1, y1
    while true do
        if x >= 0 and x < 320 and y >= 0 and y < 240 then
            ScreenPlot(x, y, colour)
        end
        if x == x2 and y == y2 then break end
        local e2 = 2 * err
        if e2 >= dy then err = err + dy x = x + sx end
        if e2 <= dx then err = err + dx y = y + sy end
    end
    return true
end

--- Screen.PixelRect(x, y, w, h, colour [, filled])
-- Mode 10: a rectangle outline (or filled when `filled` is true) of
-- pixels in a palette colour.
function Screen.PixelRect(x, y, w, h, colour, filled)
    for py = y, y + h - 1 do
        for px = x, x + w - 1 do
            if filled or py == y or py == y + h - 1 or px == x or px == x + w - 1 then
                if px >= 0 and px < 320 and py >= 0 and py < 240 then
                    ScreenPlot(px, py, colour)
                end
            end
        end
    end
    return true
end

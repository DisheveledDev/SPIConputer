-- SDK: Overlay
-- Summary: Dialogs, menus and pop-ups on the overlay layer, over the base screen.
-- Namespaces: Overlay
--
-- Same format contract as screen.lua (preamble, `function Name.Sub`
-- blocks stripped when unused, `---` signature lines).
--
-- The overlay is one layer composited over the base: cells you write
-- show, cells you never touch stay transparent, and Overlay.Clear()
-- hides everything again. Region calls take inclusive corners
-- (x1, y1, x2, y2).

Overlay = Overlay or {}
Overlay.COLS = 40      -- cells across in the current mode (Screen.Mode updates)
Overlay.ROWS = 30      -- cells down in the current mode
Overlay.SINGLE = 1
Overlay.DOUBLE = 2
Overlay.INVERT = 0x80

local cursor_row = 0

local function region(x1, y1, x2, y2)
    if x2 < x1 then x1, x2 = x2, x1 end
    if y2 < y1 then y1, y2 = y2, y1 end
    return x1, y1, x2 - x1 + 1, y2 - y1 + 1
end

--- Overlay.Clear()
-- Hides the whole overlay: every cell becomes transparent again.
function Overlay.Clear()
    return OverlayClear(32)
end

--- Overlay.Out(x, y, char [, attr])
-- Writes one character code at a cell of the overlay.
function Overlay.Out(x, y, char, attr)
    return OverlayOut(x, y, char, attr or 0)
end

--- Overlay.Attr(x, y, attr)
-- Sets one overlay cell's attribute (bit 6 set hides the cell again).
function Overlay.Attr(x, y, attr)
    return OverlayAttr(x, y, attr)
end

--- Overlay.OutText(x, y, text [, attr])
-- Writes a string on the overlay from (x, y), wrapping at the right edge.
function Overlay.OutText(x, y, text, attr)
    return OverlayWrite(x, y, tostring(text), attr or 0)
end

--- Overlay.OutAttrs(x, y, attrs)
-- Sets the attributes of consecutive overlay cells from (x, y), one
-- byte of `attrs` per cell; characters stay. One display op.
function Overlay.OutAttrs(x, y, attrs)
    return OverlayWriteAttr(x, y, attrs)
end

--- Overlay.CenterText(y, text [, attr])
-- Writes a string centred on row `y` of the overlay.
function Overlay.CenterText(y, text, attr)
    text = tostring(text)
    local x = (Overlay.COLS - #text) // 2
    if x < 0 then x = 0 end
    return OverlayWrite(x, y, text, attr or 0)
end

--- Overlay.RightText(y, text [, attr])
-- Writes a string ending at the right edge of row `y` of the overlay.
function Overlay.RightText(y, text, attr)
    text = tostring(text)
    local x = Overlay.COLS - #text
    if x < 0 then x = 0 end
    return OverlayWrite(x, y, text, attr or 0)
end

--- Overlay.Printf(x, y, format, ...)
-- string.format, written on the overlay at (x, y).
function Overlay.Printf(x, y, format, ...)
    return OverlayWrite(x, y, string.format(format, ...), 0)
end

--- Overlay.OutLine(text [, attr])
-- Writes `text` on the overlay's cursor row (row 0 to start with) and
-- moves to the next row; wraps to the top when it runs off the bottom.
function Overlay.OutLine(text, attr)
    text = tostring(text)
    local rows = 1 + (#text - 1) // Overlay.COLS
    if #text == 0 then rows = 1 end
    if cursor_row + rows > Overlay.ROWS then cursor_row = 0 end
    local ok, err = OverlayWrite(0, cursor_row, text, attr or 0)
    cursor_row = cursor_row + rows
    return ok, err
end

--- Overlay.Cursor(row)
-- Sets the row the next Overlay.OutLine writes on. Returns the row.
function Overlay.Cursor(row)
    if row then
        if row < 0 then row = 0 end
        if row >= Overlay.ROWS then row = Overlay.ROWS - 1 end
        cursor_row = row
    end
    return cursor_row
end

--- Overlay.Clean(x1, y1, x2, y2)
-- Makes the cells between two corners transparent again.
function Overlay.Clean(x1, y1, x2, y2)
    local x, y, w, h = region(x1, y1, x2, y2)
    return OverlayFill(x, y, w, h, 32, 0x40)
end

--- Overlay.Fill(x1, y1, x2, y2, char [, attr])
-- Fills the cells between two corners with a character and attribute.
function Overlay.Fill(x1, y1, x2, y2, char, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    return OverlayFill(x, y, w, h, char, attr or 0)
end

--- Overlay.FillAttr(x1, y1, x2, y2, attr)
-- Sets the attribute of every overlay cell between two corners.
function Overlay.FillAttr(x1, y1, x2, y2, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    return OverlayFillAttr(x, y, w, h, attr)
end

--- Overlay.HLine(x1, x2, y [, char [, attr]])
-- A horizontal line on the overlay (default: the box-drawing horizontal).
function Overlay.HLine(x1, x2, y, char, attr)
    local x, _, w = region(x1, y, x2, y)
    return OverlayFill(x, y, w, 1, char or 196, attr or 0)
end

--- Overlay.VLine(x, y1, y2 [, char [, attr]])
-- A vertical line on the overlay (default: the box-drawing vertical).
function Overlay.VLine(x, y1, y2, char, attr)
    local _, y, _, h = region(x, y1, x, y2)
    return OverlayFill(x, y, 1, h, char or 179, attr or 0)
end

--- Overlay.Box(x1, y1, x2, y2 [, style [, attr]])
-- A frame on the overlay between two corners: Overlay.SINGLE (default)
-- or Overlay.DOUBLE.
function Overlay.Box(x1, y1, x2, y2, style, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    return OverlayBox(x, y, w, h, style or 1, attr or 0)
end

--- Overlay.Window(x1, y1, x2, y2 [, title [, style [, attr]]])
-- A blank window on the overlay: inside cleared, frame, optional title.
function Overlay.Window(x1, y1, x2, y2, title, style, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    attr = attr or 0
    OverlayFill(x, y, w, h, 32, attr)
    local ok, err = OverlayBox(x, y, w, h, style or 1, attr)
    if ok and title and #tostring(title) > 0 then
        local label = " " .. tostring(title) .. " "
        local tx = x + (w - #label) // 2
        if tx < x + 1 then tx = x + 1 end
        OverlayWrite(tx, y, label:sub(1, w - 2), attr)
    end
    return ok, err
end

--- Overlay.Dialog(title, lines [, attr [, style]])
-- A centred dialog: a window sized to fit `lines` (a string or an array
-- of strings) with `title` on its frame. Returns the window's corners
-- (x1, y1, x2, y2) so buttons or a cursor can be placed inside it.
function Overlay.Dialog(title, lines, attr, style)
    if type(lines) == "string" then
        lines = { lines }
    end
    attr = attr or Overlay.INVERT
    local width = #tostring(title or "") + 2
    for _, line in ipairs(lines) do
        if #tostring(line) > width then width = #tostring(line) end
    end
    width = width + 4
    if width > Overlay.COLS then width = Overlay.COLS end
    local height = #lines + 4
    if height > Overlay.ROWS then height = Overlay.ROWS end
    local x1 = (Overlay.COLS - width) // 2
    local y1 = (Overlay.ROWS - height) // 2
    local x2, y2 = x1 + width - 1, y1 + height - 1
    Overlay.Window(x1, y1, x2, y2, title, style or Overlay.DOUBLE, attr)
    for i, line in ipairs(lines) do
        if y1 + 1 + i < y2 then
            OverlayWrite(x1 + 2, y1 + 1 + i, tostring(line):sub(1, width - 4), attr)
        end
    end
    return x1, y1, x2, y2
end

--- Overlay.Copy(x1, y1, x2, y2, x3, y3)
-- Copies an overlay block so its top-left lands at (x3, y3).
function Overlay.Copy(x1, y1, x2, y2, x3, y3)
    local x, y, w, h = region(x1, y1, x2, y2)
    return OverlayCopy(x, y, w, h, x3, y3)
end

--- Overlay.Move(x1, y1, x2, y2, x3, y3)
-- Moves an overlay block so its top-left lands at (x3, y3); the cells
-- it uncovers become transparent.
function Overlay.Move(x1, y1, x2, y2, x3, y3)
    local x, y, w, h = region(x1, y1, x2, y2)
    return OverlayMove(x, y, w, h, x3, y3, 32, 0x40)
end

--- Overlay.Scroll(x1, y1, x2, y2, dx, dy [, char [, attr]])
-- Shifts an overlay region by (dx, dy) cells; uncovered cells get `char`
-- and `attr` (default: transparent).
function Overlay.Scroll(x1, y1, x2, y2, dx, dy, char, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    return OverlayScroll(x, y, w, h, dx, dy, char or 32, attr or 0x40)
end

--- Overlay.OutLines(x, y, lines [, attr])
-- Writes an array of strings on consecutive overlay rows from (x, y).
-- Returns the number of rows written.
function Overlay.OutLines(x, y, lines, attr)
    local n = 0
    for i, line in ipairs(lines) do
        local row = y + i - 1
        if row >= Overlay.ROWS then break end
        OverlayWrite(x, row, tostring(line), attr or 0)
        n = n + 1
    end
    return n
end

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

--- Overlay.OutWrapped(x, y, width, text [, attr])
-- Word-wraps `text` to `width` cells on the overlay from (x, y).
-- Returns the number of rows used.
function Overlay.OutWrapped(x, y, width, text, attr)
    local lines = wrap_words(text, width)
    local n = 0
    for i, line in ipairs(lines) do
        local row = y + i - 1
        if row >= Overlay.ROWS then break end
        OverlayFill(x, row, width, 1, 32, attr or 0)
        OverlayWrite(x, row, line, attr or 0)
        n = n + 1
    end
    return n
end

--- Overlay.Label(x, y, width, text, align [, attr])
-- Writes `text` in a `width`-cell field of the overlay, aligned "left",
-- "center" or "right" (default left); longer text is cut.
function Overlay.Label(x, y, width, text, align, attr)
    text = tostring(text)
    if #text > width then text = text:sub(1, width) end
    local space = width - #text
    local before = 0
    if align == "center" then before = space // 2
    elseif align == "right" then before = space end
    local padded = string.rep(" ", before) .. text .. string.rep(" ", space - before)
    return OverlayWrite(x, y, padded, attr or 0)
end

--- Overlay.Progress(x, y, width, fraction [, attr])
-- A progress bar on the overlay: solid blocks for `fraction` (0..1),
-- light shade for the rest.
function Overlay.Progress(x, y, width, fraction, attr)
    if fraction < 0 then fraction = 0 end
    if fraction > 1 then fraction = 1 end
    local filled = math.floor(width * fraction + 0.5)
    if filled > 0 then OverlayFill(x, y, filled, 1, 219, attr or 0) end
    if filled < width then OverlayFill(x + filled, y, width - filled, 1, 176, attr or 0) end
    return true
end

--- Overlay.Rect(x1, y1, x2, y2, char [, attr])
-- The outline of a rectangle on the overlay drawn with one character.
function Overlay.Rect(x1, y1, x2, y2, char, attr)
    local x, y, w, h = region(x1, y1, x2, y2)
    attr = attr or 0
    OverlayFill(x, y, w, 1, char, attr)
    OverlayFill(x, y + h - 1, w, 1, char, attr)
    OverlayFill(x, y, 1, h, char, attr)
    return OverlayFill(x + w - 1, y, 1, h, char, attr)
end

--- Overlay.Line(x1, y1, x2, y2, char [, attr])
-- A straight line of `char` cells on the overlay (Bresenham).
function Overlay.Line(x1, y1, x2, y2, char, attr)
    attr = attr or 0
    local dx, dy = math.abs(x2 - x1), -math.abs(y2 - y1)
    local sx, sy = x1 < x2 and 1 or -1, y1 < y2 and 1 or -1
    local err = dx + dy
    local x, y = x1, y1
    while true do
        if x >= 0 and x < Overlay.COLS and y >= 0 and y < Overlay.ROWS then
            OverlayOut(x, y, char, attr)
        end
        if x == x2 and y == y2 then break end
        local e2 = 2 * err
        if e2 >= dy then err = err + dy x = x + sx end
        if e2 <= dx then err = err + dx y = y + sy end
    end
    return true
end

--- Overlay.Shade(x1, y1, x2, y2, level [, attr])
-- Fills an overlay region with a shade: level 0 (blank) to 4 (solid).
function Overlay.Shade(x1, y1, x2, y2, level, attr)
    local glyphs = { [0] = 32, 176, 177, 178, 219 }
    local x, y, w, h = region(x1, y1, x2, y2)
    return OverlayFill(x, y, w, h, glyphs[math.max(0, math.min(4, math.floor(level)))], attr or 0)
end

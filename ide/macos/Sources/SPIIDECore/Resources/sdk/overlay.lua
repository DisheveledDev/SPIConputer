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
Overlay.COLS = 40
Overlay.ROWS = 30
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

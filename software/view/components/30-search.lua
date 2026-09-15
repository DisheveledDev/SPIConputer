-- Search: an input dialog on the overlay, then a chunked scan of the
-- file (lower-cased, overlapping chunks so a match across a boundary is
-- still found), wrapping round to the top once.

local DLG_X1, DLG_Y1, DLG_X2, DLG_Y2 = 4, 11, 35, 16
local FIELD_W = DLG_X2 - DLG_X1 - 3
local DLG_ATTR = Attributes.White + Attributes.Inverse

-- The field: the tail of the answer and a cursor block, one op.
local function draw_field()
    Overlay.Label(DLG_X1 + 2, DLG_Y1 + 2, FIELD_W, answer:sub(-(FIELD_W - 1)) .. "\219",
                  "left", Attributes.Normal)
end

local function open_search()
    asking = true
    answer = ""
    Overlay.Window(DLG_X1, DLG_Y1, DLG_X2, DLG_Y2, "FIND", Overlay.DOUBLE, DLG_ATTR)
    Overlay.OutText(DLG_X1 + 2, DLG_Y2 - 1, "RETURN find   ESC cancel", DLG_ATTR)
    draw_field()
end

local function close_search()
    asking = false
    Overlay.Clear()
end

-- Byte offset of the first match at or after `from`, or nil. 1 KB at a
-- time: each chunk briefly exists three times (read buffer, string,
-- lower-case copy), and the heap is shared with the program's code.
local function scan(from, to)
    local overlap = #needle - 1
    local pos = from
    while pos < to do
        file:seek(pos)
        local chunk = file:read(math.min(1024, to - pos + overlap))
        if not chunk or #chunk == 0 then return nil end
        local hit = chunk:lower():find(needle, 1, true)
        if hit then return pos + hit - 1 end
        if pos + #chunk >= to then return nil end
        pos = pos + #chunk - overlap
    end
    return nil
end

-- Highlight line k: repaint the old and new match rows if on screen
-- (one attribute op each), else move the page so k is near the top.
local function show_match(k)
    local old = match_line
    match_line = k
    if k >= top and k <= top + H - 1 then
        if old and old >= top and old <= top + H - 1 then
            Screen.FillAttr(0, TOP + old - top, COLS - 1, TOP + old - top, Attributes.Normal)
        end
        Screen.FillAttr(0, TOP + k - top, COLS - 1, TOP + k - top, MATCH_ATTR)
    else
        top = math.max(1, math.min(k - 2, max_top()))
        draw_page()
    end
end

-- Find the next match after line `after` (0 = from the top).
local function find_next(after)
    if needle == "" then return end
    if indexing then
        draw_status("still reading the file - try again")
        return
    end
    local from = line_start(after + 1)
    local hit = scan(from, size)
    local wrapped = false
    if not hit and from > 0 then
        hit = scan(0, from)
        wrapped = true
    end
    if not hit then
        draw_status("not found: " .. needle)
        return
    end
    local k = line_at(hit)
    show_match(k)
    draw_status(string.format("line %d%s   N next", k, wrapped and " (from top)" or ""))
end

local function search_key(key)
    if key == Input.KEY_ESCAPE then
        close_search()
    elseif key == Input.KEY_RETURN then
        close_search()
        if #answer > 0 then
            needle = answer:lower()
            find_next(match_line or (top - 1))
        end
    elseif key == Input.KEY_BACKSPACE then
        answer = answer:sub(1, -2)
        draw_field()
    elseif key >= 32 and key < 127 and #answer < 60 then
        answer = answer .. string.char(key)
        draw_field()
    end
end

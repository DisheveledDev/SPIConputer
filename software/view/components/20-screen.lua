-- Drawing: the title, the status line and the text rows. Every row is
-- one Screen.Label (text, padding and attribute in a single op).

local function display_name()
    return (path:gsub("^/data", ""))
end

local function draw_title()
    local last = math.min(top + H - 1, lines)
    local where
    if lines == 0 then
        where = indexing and "reading" or "empty"
    else
        local pct = indexing and "..." or (math.floor(last * 100 / lines) .. "%")
        where = string.format("%d-%d/%d%s %s", top, last, lines,
                              (indexing or truncated) and "+" or "", pct)
    end
    local room = COLS - #where - 3
    Screen.Label(0, 0, COLS, string.format(" %-" .. room .. "." .. room .. "s %s", display_name(), where),
                 "left", TITLE_ATTR)
end

local function draw_status(message)
    Screen.Label(0, STATUS_ROW, COLS, " " .. (message or HELP), "left", STATUS_ATTR)
end

-- Row r (0-based in the text area) showing line k (or blank past the end).
local function draw_row(r, k, text)
    local attr = (k == match_line) and MATCH_ATTR or Attributes.Normal
    Screen.Label(0, TOP + r, COLS, text or "", "left", attr)
end

local function draw_page()
    local texts = read_lines(top, H)
    for r = 0, H - 1 do
        local k = top + r
        draw_row(r, k, k <= lines and texts[r + 1] or nil)
    end
    draw_title()
end

-- Largest first line that still fills the screen.
local function max_top()
    return math.max(1, lines - H + 1)
end

-- Move the view to `new_top`: one Screen.Scroll plus one row for a
-- single-line step, a full page otherwise.
local function scroll_to(new_top)
    new_top = math.max(1, math.min(new_top, max_top()))
    local d = new_top - top
    if d == 0 then return end
    top = new_top
    if d == 1 then
        Screen.Scroll(0, TOP, COLS - 1, TOP + H - 1, 0, -1)
        local k = top + H - 1
        draw_row(H - 1, k, k <= lines and read_lines(k, 1)[1] or nil)
        draw_title()
    elseif d == -1 then
        Screen.Scroll(0, TOP, COLS - 1, TOP + H - 1, 0, 1)
        draw_row(0, top, read_lines(top, 1)[1])
        draw_title()
    else
        draw_page()
    end
end

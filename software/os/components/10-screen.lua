-- Screen buffer: the shell keeps every output line and repaints the
-- tile map, so scrolling, resuming after a program and the blinking
-- cursor are all just a repaint of `lines` + `input`.

local lines = {}          -- committed output lines (one screen row each)
local input = ""          -- the line being typed
local caret_col = 0       -- 0-based caret position within `input`
local prompt_row = 0      -- screen row the input line sits on
local cursor_on = true
local last_blink = 0
local needs_repaint = false -- a program ran; redraw on the next tick

local function cursor_cell()
    local ch = 32
    if caret_col < #input then
        ch = input:byte(caret_col + 1)
    end
    return ch
end

local function paint_cursor()
    if prompt_row >= ROWS then
        return
    end
    local cx = math.min(caret_col, COLS - 1)
    ScreenOut(cx, prompt_row, cursor_cell(), cursor_on and 0x80 or 0)
end

local function paint()
    ScreenClear(32)
    for i = 1, math.min(#lines, ROWS) do
        local line = lines[i]
        for c = 1, math.min(#line, COLS) do
            ScreenOut(c - 1, i - 1, line:byte(c))
        end
    end
    prompt_row = #lines
    for c = 1, math.min(#input, COLS) do
        if prompt_row < ROWS then
            ScreenOut(c - 1, prompt_row, input:byte(c))
        end
    end
    paint_cursor()
end

local function commit(line)
    lines[#lines + 1] = line
    if #lines > ROWS - 1 then
        table.remove(lines, 1)
    end
end

-- Write a message; "\n" separates lines.
local function out(message)
    for part in (tostring(message) .. "\n"):gmatch("([^\n]*)\n") do
        commit(part)
    end
    paint()
end

-- Terminal: output rows, the pager and the input row.
--
-- Output is queued (`pending`) and drained a screen at a time by flush().
-- A queued item is a line or a producer ({ next = fn, close = fn }) that
-- yields lines on demand, so TYPE streams a large file through the pager
-- without holding it. With a redirection active, output goes to `sink`
-- (a file) instead of the queue.

local row = 0              -- the row the next line (or the input) uses
local input = ""           -- the command line being typed
local caret = 0            -- 0-based caret position in `input`
local view = 0             -- first input character shown (long lines scroll)
local cursor_on = true
local cursor_col = nil     -- column the cursor attribute is on, nil when hidden
local pending, pending_head = {}, 1
local page_count = 0       -- rows shown since the user last pressed a key
local more_waiting = false -- the pager waits for a key
local sink = nil           -- redirection target: function(line)

local function advance()
    if row < ROWS - 1 then
        row = row + 1
    else
        Screen.ScrollUp(1)
    end
end

-- One finished row. Label pads to the full width with attribute 0, so it
-- replaces whatever the row held, a cursor or --MORE-- included.
local function put_row(text)
    Screen.Label(0, row, COLS, text, "left", 0)
    advance()
end

-- Queue a line or a producer (or write it straight to the redirection).
local function emit(item)
    if not sink then
        pending[#pending + 1] = item
    elseif type(item) == "string" then
        sink(item)
    else
        for line in item.next do sink(line) end
        if item.close then item.close() end
    end
end

-- Write a message; "\n" separates lines.
local function out(message)
    message = tostring(message)
    if not message:find("\n", 1, true) then
        emit(message)
        return
    end
    for part in (message .. "\n"):gmatch("([^\n]*)\n") do
        emit(part)
    end
end

-- Errors always reach the screen, even while output is redirected.
local function out_error(message)
    pending[#pending + 1] = "?" .. tostring(message)
end

local function next_line()
    while pending_head <= #pending do
        local item = pending[pending_head]
        if type(item) == "string" then
            pending[pending_head] = nil
            pending_head = pending_head + 1
            return item
        end
        local line = item.next()
        if line then return line end
        if item.close then item.close() end
        pending[pending_head] = nil
        pending_head = pending_head + 1
    end
    pending, pending_head = {}, 1
    return nil
end

local function discard_output()
    for i = pending_head, #pending do
        local item = pending[i]
        if type(item) == "table" and item.close then item.close() end
    end
    pending, pending_head = {}, 1
end

-- Show queued output. Returns true when all of it is on screen, false
-- when it stopped at a full page (the pager then waits for a key).
local function flush()
    while true do
        if page_count >= ROWS - 1 and pending_head <= #pending then
            more_waiting = true
            Screen.Label(0, row, COLS, "-- MORE --  SPACE RETURN ESC", "left", INVERSE)
            return false
        end
        local line = next_line()
        if not line then return true end
        if #line <= COLS then
            put_row(line)
            page_count = page_count + 1
        else
            for i = 1, #line, COLS do
                put_row(line:sub(i, i + COLS - 1))
                page_count = page_count + 1
            end
        end
    end
end

-- The input row: one Label op, plus the cursor's attribute.
local function draw_input()
    if caret < view then
        view = caret
    elseif caret - view > COLS - 1 then
        view = caret - (COLS - 1)
    end
    Screen.Label(0, row, COLS, input:sub(view + 1, view + COLS), "left", 0)
    cursor_col = caret - view
    if cursor_on then Screen.Attr(cursor_col, row, INVERSE) end
end

-- Leave the input row as plain text and move below it.
local function commit_input()
    cursor_col = nil
    put_row(input:sub(view + 1, view + COLS))
end

local function blink()
    cursor_on = not cursor_on
    if cursor_col then
        Screen.Attr(cursor_col, row, cursor_on and INVERSE or 0)
    end
end

local function start_input()
    input, caret, view = "", 0, 0
    cursor_on = true
    draw_input()
end

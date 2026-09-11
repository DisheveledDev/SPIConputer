-- editor.lua
-- SPIComputer OS text editor (Phase 6, the first real application).
-- Launch from the shell:  run editor.lua <file>   (or: edit <file>)
--
-- The file is edited entirely in RAM (array of lines) and written back
-- on save, so insertion is fast and the SD card only sees whole-file
-- writes. Files are capped at 128 KB by fs.readall.
--
-- Keys:
--   cursor keys (or ESC [ A/B/C/D from the terminal)  move
--   Return        split the line
--   Backspace     delete before cursor
--   Shift+Del     insert a space (C64 INST semantics)
--   Forward del   delete at cursor
--   Home          start of line
--   Ctrl+S        save
--   Ctrl+Q        quit (asks if the buffer is dirty)
--   printable     insert (shift is already applied by the OS)

local filename = ... or "untitled.txt"

local W, H = 40, 29        -- text area; screen line 29 is the status bar
local lines = {}
local cx, cy = 0, 1        -- cursor: column (0-based), line (1-based)
local scroll_y = 0         -- top visible line (0-based index)
local dirty = false
local quit_confirm = false
local cursor_on = true
local esc_seq = ""         -- ANSI escape sequence assembler

-- ---------------------------------------------------------------- load

local function load_content(content)
    if content == "" then return { "" } end
    local base = content
    if content:sub(-1) ~= "\n" then base = content .. "\n" end
    local t, i = {}, 0
    for l in base:gmatch("([^\n]*)\n") do i = i + 1; t[i] = l end
    return t
end

-- ---------------------------------------------------------------- draw

local function draw_cursor()
    local ly = cy - 1 - scroll_y
    if ly >= 0 and ly < H and cx < W then
        local ch = (lines[cy] or ""):byte(cx + 1) or 32
        ScreenOut(cx, ly, ch, cursor_on and 0x80 or 0)
    end
end

local function draw_status()
    local s = string.format("%s L%d C%d%s", filename, cy, cx + 1,
                            dirty and " *" or "")
    if quit_confirm then s = "save? (y/n)  " .. s end
    for c = 1, W do
        ScreenOut(c - 1, H, s:byte(c) or 32, 0x80)
    end
end

local function draw()
    ScreenClear(32)
    for r = 0, H - 1 do
        local li = lines[scroll_y + r + 1]
        if li then
            for c = 1, W do
                local b = li:byte(c)
                if b then ScreenOut(c - 1, r, b, 0) end
            end
        end
    end
    draw_cursor()
    draw_status()
end

local function save()
    local ok, err = fs.writeall(filename, table.concat(lines, "\n") .. "\n")
    if ok then
        dirty = false
    else
        print("save failed: " .. tostring(err))
    end
    draw_status()
end

local function quit()
    ExitProgram()
end

-- ---------------------------------------------------------------- edits

local function insert_char(ch)
    local line = lines[cy]
    lines[cy] = line:sub(1, cx) .. ch .. line:sub(cx + 1)
    cx = cx + 1
    dirty = true
    draw()
end

local function backspace()
    if cx > 0 then
        local line = lines[cy]
        lines[cy] = line:sub(1, cx - 1) .. line:sub(cx + 1)
        cx = cx - 1
    elseif cy > 1 then
        local prev = lines[cy - 1]
        lines[cy - 1] = prev .. lines[cy]
        table.remove(lines, cy)
        cx = #prev
        cy = cy - 1
    end
    dirty = true
    draw()
end

local function fwd_delete()
    local line = lines[cy]
    if cx < #line then
        lines[cy] = line:sub(1, cx) .. line:sub(cx + 2)
    elseif cy < #lines then
        lines[cy] = line .. lines[cy + 1]
        table.remove(lines, cy + 1)
    end
    dirty = true
    draw()
end

local function insert_newline()
    local line = lines[cy]
    lines[cy] = line:sub(1, cx)
    table.insert(lines, cy + 1, line:sub(cx + 1))
    cx = 0
    cy = cy + 1
    if cy - 1 >= scroll_y + H then scroll_y = scroll_y + 1 end
    dirty = true
    draw()
end

local function move(dx, dy)
    cx = cx + dx
    cy = cy + dy
    cx = math.max(0, math.min(cx, W - 1))
    cy = math.max(1, math.min(cy, #lines))
    local ll = #lines[cy]
    if cx > ll then cx = ll end
    if cy - 1 < scroll_y then
        scroll_y = cy - 1
    elseif cy - 1 >= scroll_y + H then
        scroll_y = cy - H + 1
    end
    draw()
end

-- ---------------------------------------------------------------- input

local function handle_key(ev)
    if quit_confirm then
        if ev.key == 121 then save(); quit()       -- y
        elseif ev.key == 110 then quit()           -- n
        else quit_confirm = false; draw_status() end
        return
    end

    -- ANSI arrow sequences from the terminal (ESC [ A/B/C/D).
    if ev.key == 27 then esc_seq = "ESC" return end
    if esc_seq == "ESC" then
        if ev.key == 91 then esc_seq = "ESC[" return end
        esc_seq = ""
    end
    if esc_seq == "ESC[" then
        esc_seq = ""
        if ev.key == 65 then move(0, -1)
        elseif ev.key == 66 then move(0, 1)
        elseif ev.key == 67 then move(1, 0)
        elseif ev.key == 68 then move(-1, 0) end
        return
    end

    local k = ev.key
    if k == 128 then move(0, -1)
    elseif k == 129 then move(0, 1)
    elseif k == 130 then move(-1, 0)
    elseif k == 131 then move(1, 0)
    elseif k == 13 then insert_newline()
    elseif k == 8 then
        if ev.mods % 2 == 1 then insert_char(" ") else backspace() end
    elseif k == 127 then fwd_delete()
    elseif k == 19 then save()
    elseif k == 17 then
        if dirty then quit_confirm = true; draw_status() else quit() end
    elseif k == 139 then cx = 0; draw()            -- Home
    elseif k >= 32 and k < 128 then insert_char(string.char(k))
    end
end

-- ---------------------------------------------------------------- entry

function setup()
    ScreenMode(1)
    local content, err = fs.readall(filename)
    if content then
        lines = load_content(content)
    else
        lines = { "" }
        print("new file: " .. tostring(err))
    end
    TimerCreate(function()
        cursor_on = not cursor_on
        draw_cursor()
    end, 500)
    draw()
end

function tick()
    while true do
        local ev = InputPoll()
        if not ev then break end
        if ev.type == "key" and ev.pressed == 1 then handle_key(ev) end
    end
end

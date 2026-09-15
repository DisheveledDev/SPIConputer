-- Editing operations: each mutates the buffer and redraws what changed.

-- Keep the cursor line on screen; returns true when the view scrolled.
local function ensure_visible()
    if cy - 1 < scroll_y then
        scroll_y = cy - 1
        return true
    elseif cy - 1 >= scroll_y + H then
        scroll_y = cy - H + 1
        return true
    end
    return false
end

local function insert_char(ch)
    local line = lines[cy]
    lines[cy] = line:sub(1, cx) .. ch .. line:sub(cx + 1)
    cx = cx + 1
    dirty = true
    draw_text()
    draw_status()
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
        ensure_visible()
    end
    dirty = true
    draw_text()
    draw_status()
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
    draw_text()
    draw_status()
end

local function insert_newline()
    local line = lines[cy]
    lines[cy] = line:sub(1, cx)
    table.insert(lines, cy + 1, line:sub(cx + 1))
    cx = 0
    cy = cy + 1
    ensure_visible()
    dirty = true
    draw_text()
    draw_status()
end

local function move(dx, dy)
    -- Put the old cursor cell back before moving the attribute.
    local row = cursor_row()
    if row >= TEXT_TOP and row <= TEXT_TOP + H - 1 and cx < COLS then
        Screen.Attr(cx, row, 0)
    end
    cx = cx + dx
    cy = cy + dy
    cx = math.max(0, math.min(cx, COLS - 1))
    cy = math.max(1, math.min(cy, #lines))
    local ll = #lines[cy]
    if cx > ll then cx = ll end
    if ensure_visible() then
        draw_text()
    else
        draw_cursor()
    end
    draw_status()
end

local function goto_line(target)
    cy = math.max(1, math.min(math.floor(target), #lines))
    cx = 0
    ensure_visible()
    draw_text()
    draw_status()
end

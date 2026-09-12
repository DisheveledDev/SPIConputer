-- Editing operations: each mutates the buffer and redraws.

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

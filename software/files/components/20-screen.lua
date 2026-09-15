-- Drawing: one Screen.Label per row; the selection is a FillAttr.

local function draw_title()
    local free = fs.free()
    local space = free and (Text.Commas(free // 1024) .. " MB free") or ""
    Screen.Label(0, 0, COLS, string.format(" FILES %-18.18s %13s ", shown(cwd), space),
                 "left", TITLE_ATTR)
end

local function draw_frame()
    Screen.Label(0, 1, COLS, string.format(" %-27s %10s ", "NAME", "SIZE"), "left", HEAD_ATTR)
    Screen.Label(0, KEYS_ROW, COLS, "RET Open E Edit C Copy R Ren D Del M Dir", "left", KEYS_ATTR)
end

local function draw_info()
    local dirs, files, bytes = 0, 0, 0
    for i = 1, count do
        local size = sizes[i]
        if size == FOLDER then
            dirs = dirs + 1
        elseif size >= 0 then
            files = files + 1
            bytes = bytes + size
        end
    end
    Screen.Label(0, INFO_ROW, COLS, string.format(" %s, %s, %s bytes", Text.Plural(dirs, "folder"),
                 Text.Plural(files, "file"), Text.Commas(bytes)), "left", INFO_ATTR)
end

-- Entry i on its row (if it is on screen), selected or not.
local function draw_row(i)
    local r = i - top
    if r < 0 or r >= LIST_H then return end
    if i > count then
        Screen.Clean(0, LIST_TOP + r, COLS - 1, LIST_TOP + r)
        return
    end
    local attr = entry_attr(i) + (i == sel and Attributes.Inverse or 0)
    Screen.Label(0, LIST_TOP + r, COLS, row_text(i), "left", attr)
end

local function set_row_attr(i)
    local r = i - top
    if r < 0 or r >= LIST_H or i > count then return end
    local attr = entry_attr(i) + (i == sel and Attributes.Inverse or 0)
    Screen.FillAttr(0, LIST_TOP + r, COLS - 1, LIST_TOP + r, attr)
end

local function draw_list()
    for i = top, top + LIST_H - 1 do draw_row(i) end
    if count == 0 or (count == 1 and sizes[1] == UP) then
        Screen.Label(0, LIST_TOP + count + 1, COLS, "(empty folder)", "center", FILE_ATTR)
    end
end

local function draw_all()
    draw_title()
    draw_list()
    draw_info()
end

-- Select entry `new`: two attribute ops on screen, or one scroll and a
-- row when it steps just past the edge, or a full list otherwise.
local function choose(new)
    new = math.max(1, math.min(new, count))
    if new == sel or count == 0 then return end
    local old = sel
    sel = new
    if sel >= top and sel < top + LIST_H then
        set_row_attr(old)
        set_row_attr(sel)
        return
    end
    local new_top = sel < top and sel or sel - LIST_H + 1
    local d = new_top - top
    top = new_top
    if d == 1 or d == -1 then
        Screen.Scroll(0, LIST_TOP, COLS - 1, LIST_TOP + LIST_H - 1, 0, -d)
        set_row_attr(old)
        draw_row(sel)
    else
        draw_list()
    end
end

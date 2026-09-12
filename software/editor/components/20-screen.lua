-- Screen: 29 text lines plus an inverted status bar (line 29), and the
-- blinking cursor cell.

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

local function fill_rect(x, y, w, h, attr)
    for row = y, y + h - 1 do
        for col = x, x + w - 1 do
            ScreenOut(col, row, 32, attr)
        end
    end
end

local function draw_string(x, y, text, attr)
    for i = 1, #text do
        if x + i - 1 < W and y >= 0 and y <= H then
            ScreenOut(x + i - 1, y, text:byte(i), attr)
        end
    end
end

local function menu_x(index)
    local x = 0
    for i = 1, index - 1 do
        x = x + #menu_defs[i].name + 2
    end
    return x
end

local function menu_width(menu)
    local width = #menu.name + 2
    for _, item in ipairs(menu.items) do
        width = math.max(width, #item + 2)
    end
    return width
end

local function draw_menu_layer()
    ScreenZOrder(1)
    ScreenClear(32)
    for i, menu in ipairs(menu_defs) do
        local x = menu_x(i)
        local attr = i == menu_top and 0x80 or 0
        for col = x, x + #menu.name + 1 do
            ScreenOut(col, 0, 32, attr)
        end
        draw_string(x + 1, 0, menu.name, attr)
    end
    if menu_open then
        local menu = menu_defs[menu_top]
        local x = menu_x(menu_top)
        local width = menu_width(menu)
        fill_rect(x, 1, width, #menu.items, 0)
        for i, item in ipairs(menu.items) do
            local attr = i == menu_item and 0x80 or 0
            for col = x, x + width - 1 do
                ScreenOut(col, i, 32, attr)
            end
            draw_string(x + 1, i, item, attr)
        end
    end
end

local function draw_dialog_layer()
    ScreenZOrder(2)
    ScreenClear(32)
    if not dialog_open then return end
    if dialog_kind == "goto" then
        local x = math.floor((W - 26) / 2)
        fill_rect(x, 10, 26, 5, 0)
        draw_string(x + 2, 11, "GO TO LINE", 0)
        draw_string(x + 2, 13, dialog_text, 0x80)
        ScreenOut(x + 2 + #dialog_text, 13, 32, 0x80)
        draw_string(x + 2, 14, "ENTER ACCEPT  ESC CANCEL", 0)
    elseif dialog_kind == "help" then
        local x = math.floor((W - 36) / 2)
        fill_rect(x, 4, 36, 21, 0)
        draw_string(x + 13, 5, "EDITOR HELP", 0x80)
        local help = {
            "CTRL+F        FILE MENU",
            "CTRL+E        EDIT MENU",
            "CTRL+O        OPTIONS MENU",
            "CTRL+H        HELP MENU",
            "OPTIONS       TOGGLE 40/80 WIDTH",
            "ARROWS        MOVE / NAVIGATE",
            "ENTER         SELECT MENU ITEM",
            "ESC           CLOSE MENU",
            "CTRL+S        SAVE",
            "CTRL+Q        QUIT",
        }
        for i, line in ipairs(help) do
            draw_string(x + 2, 7 + i, line, 0)
        end
        draw_string(x + 6, 20, "PRESS ESC TO CLOSE", 0x80)
    end
end

local function draw_overlays()
    draw_menu_layer()
    draw_dialog_layer()
    ScreenZOrder(0)
end

local function draw()
    ScreenZOrder(0)
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
    draw_overlays()
end

-- Screen: menu bar, text area and status bar on the base layer; menus
-- and dialogs on the overlay.

local function menu_x(index)
    local x = 0
    for i = 1, index - 1 do
        x = x + #menu_defs[i].name + 2
    end
    return x
end

-- The menu bar: titles with the open one highlighted.
local function draw_menu_bar()
    Screen.Clean(0, 0, COLS - 1, 0)
    for i, menu in ipairs(menu_defs) do
        local attr = (overlay_mode == "menu" and i == menu_top) and INVERT or 0
        Screen.OutText(menu_x(i), 0, " " .. menu.name .. " ", attr)
    end
end

local function draw_status()
    local s = string.format("%s  L%d C%d%s", filename:gsub("^/data/", ""), cy, cx + 1,
                            dirty and "  *" or "")
    Screen.Label(0, STATUS_ROW, COLS, s, "left", INVERT)
end

-- The cursor is the invert attribute on its cell, so blinking is two
-- single-cell ops and never a redraw.
local function cursor_row()
    return TEXT_TOP + cy - 1 - scroll_y
end

local function draw_cursor()
    local row = cursor_row()
    if row < TEXT_TOP or row > TEXT_TOP + H - 1 or cx >= COLS then return end
    local on = cursor_visible or not blink_enabled
    Screen.Attr(cx, row, on and INVERT or 0)
end

-- The text area: one Screen.OutText per visible line.
local function draw_text()
    Screen.Clean(0, TEXT_TOP, COLS - 1, TEXT_TOP + H - 1)
    for r = 0, H - 1 do
        local line = lines[scroll_y + r + 1]
        if line and #line > 0 then
            Screen.OutText(0, TEXT_TOP + r, line:sub(1, COLS))
        end
    end
    draw_cursor()
end

local function draw()
    draw_menu_bar()
    draw_text()
    draw_status()
end

-- Overlay: the open drop-down, drawn as a window under its title.
local function menu_width(menu)
    local width = #menu.name + 2
    for _, item in ipairs(menu.items) do
        width = math.max(width, #item + 4)
    end
    return width
end

local function draw_menu()
    local menu = menu_defs[menu_top]
    local x = menu_x(menu_top)
    local width = menu_width(menu)
    if x + width > COLS then x = COLS - width end
    local y2 = 1 + #menu.items + 1
    Overlay.Clear()
    Overlay.Window(x, 1, x + width - 1, y2, nil, Overlay.SINGLE, INVERT)
    for i, item in ipairs(menu.items) do
        local attr = (i == menu_item) and 0 or INVERT
        Overlay.Label(x + 1, 1 + i, width - 2, " " .. item, "left", attr)
    end
end

local HELP_LINES = {
    "F2 / CTRL+F    FILE menu",
    "F3 / CTRL+E    EDIT menu",
    "F4 / CTRL+O    OPTIONS menu",
    "F1 / CTRL+H    this help",
    "ARROWS         move, navigate menus",
    "HOME           start of line",
    "RETURN         split line / choose",
    "BACKSPACE      delete before cursor",
    "SHIFT+BACKSP.  insert a space",
    "DEL            delete at cursor",
    "CTRL+S         save",
    "CTRL+Q         quit",
    "ESC            close menu or dialog",
}

local function draw_dialog()
    Overlay.Clear()
    if overlay_mode == "goto" then
        Overlay.Dialog("GO TO LINE", {
            "Line: " .. dialog_text .. "_",
            "",
            "RETURN accept    ESC cancel",
        })
    elseif overlay_mode == "help" then
        Overlay.Dialog("EDITOR HELP", HELP_LINES)
    elseif overlay_mode == "quit" then
        Overlay.Dialog("UNSAVED CHANGES", {
            "Save " .. filename:gsub("^/data/", "") .. " before quitting?",
            "",
            "Y save and quit   N quit   ESC stay",
        })
    elseif overlay_mode == "info" then
        local bytes = 0
        for _, line in ipairs(lines) do bytes = bytes + #line + 1 end
        Overlay.Dialog("FILE INFO", {
            filename,
            Text.Plural(#lines, "line") .. ", " .. Text.Commas(bytes) .. " bytes",
            dirty and "unsaved changes" or "saved",
            "",
            "RETURN or ESC to close",
        })
    end
end

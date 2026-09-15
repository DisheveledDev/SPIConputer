-- Commands: save and quit, the menus and the dialogs.

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

-- The overlay owns the keyboard while something is on it, and the
-- cursor stops blinking so the text underneath stays still.
local function set_overlay(mode)
    overlay_mode = mode
    if mode then
        if blink_timer then blink_timer:Pause() end
        cursor_visible = true
        draw_cursor()
    else
        Overlay.Clear()
        if blink_timer then blink_timer:Resume() end
    end
    draw_menu_bar()
end

local function close_overlay()
    set_overlay(nil)
end

local function open_menu(index)
    menu_top = index
    menu_item = 1
    set_overlay("menu")
    draw_menu()
end

local function open_dialog(kind)
    dialog_text = ""
    set_overlay(kind)
    draw_dialog()
end

local function ask_quit()
    if dirty then
        open_dialog("quit")
    else
        quit()
    end
end

local function select_menu_item()
    local menu = menu_defs[menu_top]
    local item = menu.items[menu_item]
    if item == "Save" then
        close_overlay()
        save()
    elseif item == "Go to line" then
        open_dialog("goto")
    elseif item == "Quit" then
        close_overlay()
        ask_quit()
    elseif item == "Top of file" then
        close_overlay()
        goto_line(1)
    elseif item == "Bottom of file" then
        close_overlay()
        goto_line(#lines)
    elseif item == "Delete line" then
        if #lines > 1 then
            table.remove(lines, cy)
            cy = math.min(cy, #lines)
            cx = math.min(cx, #(lines[cy] or ""))
            dirty = true
        end
        close_overlay()
        draw_text()
        draw_status()
    elseif item == "Toggle 40/80 columns" then
        close_overlay()
        wide = not wide
        Screen.Mode(wide and Screen.TEXT80C or Screen.TEXT40C)
        Screen.Palette(0, 0, 0, 160)
        Screen.Palette(1, 255, 255, 255)
        COLS = Screen.COLS
        H = Screen.ROWS - 2
        STATUS_ROW = Screen.ROWS - 1
        if cx >= COLS then cx = COLS - 1 end
        ensure_visible()
        draw()
    elseif item == "Toggle cursor blink" then
        blink_enabled = not blink_enabled
        cursor_visible = true
        close_overlay()
        draw_cursor()
    elseif item == "File info" then
        open_dialog("info")
    elseif item == "Keyboard help" then
        open_dialog("help")
    end
end

local function handle_menu_key(key)
    local menu = menu_defs[menu_top]
    if key == KEY_ESCAPE then
        close_overlay()
    elseif key == KEY_UP then
        menu_item = (menu_item - 2) % #menu.items + 1
        draw_menu()
    elseif key == KEY_DOWN then
        menu_item = menu_item % #menu.items + 1
        draw_menu()
    elseif key == KEY_LEFT then
        open_menu((menu_top - 2) % #menu_defs + 1)
    elseif key == KEY_RIGHT then
        open_menu(menu_top % #menu_defs + 1)
    elseif key == KEY_RETURN then
        select_menu_item()
    end
end

local function handle_dialog_key(key)
    if key == KEY_ESCAPE then
        close_overlay()
    elseif overlay_mode == "help" or overlay_mode == "info" then
        if key == KEY_RETURN then close_overlay() end
    elseif overlay_mode == "quit" then
        if key == 121 or key == 89 then       -- y
            save()
            quit()
        elseif key == 110 or key == 78 then   -- n
            quit()
        end
    elseif overlay_mode == "goto" then
        if key == KEY_RETURN then
            local target = tonumber(dialog_text)
            close_overlay()
            if target then goto_line(target) end
        elseif key == KEY_BACKSPACE then
            dialog_text = dialog_text:sub(1, -2)
            draw_dialog()
        elseif key >= 48 and key <= 57 and #dialog_text < 5 then
            dialog_text = dialog_text .. string.char(key)
            draw_dialog()
        end
    end
end

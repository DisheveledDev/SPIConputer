-- File commands: save the RAM buffer back to the SD card, quit.

local function save()
    local ok, err = fs.writeall(filename, table.concat(lines, "\n") .. "\n")
    if ok then
        dirty = false
    else
        print("save failed: " .. tostring(err))
    end
    draw()
end

local function quit()
    ExitProgram()
end

local function close_menus()
    menu_open = false
    dialog_open = false
    dialog_kind = ""
    dialog_text = ""
    draw()
end

local function open_menu(index)
    dialog_open = false
    menu_open = true
    menu_top = index
    menu_item = 1
    draw()
end

local function open_dialog(kind)
    menu_open = false
    dialog_open = true
    dialog_kind = kind
    dialog_text = ""
    dialog_cursor = 0
    draw()
end

local function toggle_width()
    editor_wide = not editor_wide
    if editor_wide then
        W, H = 80, 59
        ScreenMode(3)
    else
        W, H = 40, 29
        ScreenMode(1)
    end
    cx = math.min(cx, W - 1)
    scroll_y = math.max(0, math.min(scroll_y, math.max(0, #lines - H)))
    ScreenPalette(0, 0, 0, 160)
    ScreenPalette(1, 255, 255, 255)
    draw()
end

local function select_menu_item()
    local menu = menu_defs[menu_top]
    local item = menu.items[menu_item]
    if menu_top == 1 and item == "Save" then
        save()
        close_menus()
    elseif menu_top == 1 and item == "Go to line" then
        open_dialog("goto")
    elseif menu_top == 1 and item == "Quit" then
        close_menus()
        if dirty then
            quit_confirm = true
            draw()
        else
            quit()
        end
    elseif menu_top == 2 and item == "Top of file" then
        cy = 1
        cx = 0
        scroll_y = 0
        close_menus()
    elseif menu_top == 2 and item == "Bottom of file" then
        cy = #lines
        cx = 0
        scroll_y = math.max(0, cy - H)
        close_menus()
    elseif menu_top == 2 and item == "Delete line" then
        if #lines > 1 then
            table.remove(lines, cy)
            cy = math.min(cy, #lines)
            cx = math.min(cx, #(lines[cy] or ""))
            dirty = true
        end
        close_menus()
    elseif menu_top == 3 and item == "Toggle 40/80" then
        toggle_width()
        close_menus()
    elseif menu_top == 3 and item == "Toggle cursor" then
        cursor_on = not cursor_on
        close_menus()
    elseif menu_top == 3 and item == "Clear menu" then
        close_menus()
    elseif menu_top == 4 and item == "Keyboard help" then
        open_dialog("help")
    end
end

local function handle_menu_key(ev)
    local k = ev.key
    if k == 27 then
        close_menus()
    elseif k == 128 then
        if menu_open then
            menu_item = (menu_item - 2) % #menu_defs[menu_top].items + 1
            draw()
        end
    elseif k == 129 then
        if menu_open then
            menu_item = menu_item % #menu_defs[menu_top].items + 1
            draw()
        end
    elseif k == 130 and menu_open then
        menu_top = (menu_top - 2) % #menu_defs + 1
        menu_item = 1
        draw()
    elseif k == 131 and menu_open then
        menu_top = menu_top % #menu_defs + 1
        menu_item = 1
        draw()
    elseif k == 13 and menu_open then
        select_menu_item()
    end
end

local function handle_dialog_key(ev)
    local k = ev.key
    if k == 27 then
        close_menus()
    elseif dialog_kind == "help" then
        if k == 13 then close_menus() end
    elseif dialog_kind == "goto" then
        if k == 13 then
            local target = tonumber(dialog_text)
            if target then
                cy = math.max(1, math.min(math.floor(target), #lines))
                cx = 0
                if cy - 1 < scroll_y then
                    scroll_y = cy - 1
                elseif cy - 1 >= scroll_y + H then
                    scroll_y = cy - H + 1
                end
            end
            close_menus()
        elseif k == 8 then
            dialog_text = dialog_text:sub(1, -2)
            draw()
        elseif k >= 48 and k <= 57 and #dialog_text < 5 then
            dialog_text = dialog_text .. string.char(k)
            draw()
        end
    end
end

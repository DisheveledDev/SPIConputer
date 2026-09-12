-- Keyboard: cursor keys, editing keys, menu shortcuts and dialogs.

local function ctrl_down(mods)
    return math.floor(mods / 2) % 2 == 1
end

local function handle_key(ev)
    if dialog_open then
        handle_dialog_key(ev)
        return
    end
    if menu_open then
        handle_menu_key(ev)
        return
    end
    if quit_confirm then
        if ev.key == 121 then save(); quit()
        elseif ev.key == 110 then quit()
        else quit_confirm = false; draw_status() end
        return
    end

    local k = ev.key
    if ctrl_down(ev.mods) then
        if k == 102 then open_menu(1)
        elseif k == 101 then open_menu(2)
        elseif k == 111 then open_menu(3)
        elseif k == 104 then open_dialog("help")
        elseif k == 115 then save()
        elseif k == 113 then
            if dirty then quit_confirm = true; draw() else quit() end
        end
        return
    end

    if k == 132 then open_dialog("help")
    elseif k == 133 then open_menu(1)
    elseif k == 134 then open_menu(2)
    elseif k == 135 then open_menu(3)
    elseif k == 128 then move(0, -1)
    elseif k == 129 then move(0, 1)
    elseif k == 130 then move(-1, 0)
    elseif k == 131 then move(1, 0)
    elseif k == 13 then insert_newline()
    elseif k == 8 then
        if ev.mods % 2 == 1 then insert_char(" ") else backspace() end
    elseif k == 127 then fwd_delete()
    elseif k == 19 then save()
    elseif k == 17 then
        if dirty then quit_confirm = true; draw() else quit() end
    elseif k == 139 then cx = 0; draw()
    elseif k >= 32 and k < 128 then insert_char(string.char(k))
    end
end

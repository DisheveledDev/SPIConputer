-- Keyboard: the OS calls on_keypress for every key-down with the
-- modifier state; the overlay takes the keys while it is showing.

function on_keypress(key, shift, ctrl)
    if overlay_mode == "menu" then
        handle_menu_key(key)
        return
    elseif overlay_mode then
        handle_dialog_key(key)
        return
    end

    if ctrl then
        if key == 102 then open_menu(1)          -- f
        elseif key == 101 then open_menu(2)      -- e
        elseif key == 111 then open_menu(3)      -- o
        elseif key == 104 then open_dialog("help") -- h
        elseif key == 115 then save()            -- s
        elseif key == 113 then ask_quit()        -- q
        end
        return
    end

    if key == KEY_F1 then open_dialog("help")
    elseif key == KEY_F2 then open_menu(1)
    elseif key == KEY_F3 then open_menu(2)
    elseif key == KEY_F4 then open_menu(3)
    elseif key == KEY_UP then move(0, -1)
    elseif key == KEY_DOWN then move(0, 1)
    elseif key == KEY_LEFT then move(-1, 0)
    elseif key == KEY_RIGHT then move(1, 0)
    elseif key == KEY_RETURN then insert_newline()
    elseif key == KEY_BACKSPACE then
        if shift then insert_char(" ") else backspace() end
    elseif key == KEY_DELETE then fwd_delete()
    elseif key == 19 then save()                 -- Ctrl+S as a control code
    elseif key == 17 then ask_quit()             -- Ctrl+Q as a control code
    elseif key == KEY_HOME then move(-cx, 0)
    elseif key >= 32 and key < 127 then insert_char(string.char(key))
    end
end

-- Keys (the OS's on_keypress, key-down only). Letters work in either
-- case.

local function files_key(key)
    if mode then
        dialog_key(key)
        return
    end
    local k = (key >= 65 and key <= 90) and key + 32 or key
    if key == Input.KEY_UP then
        choose(sel - 1)
    elseif key == Input.KEY_DOWN then
        choose(sel + 1)
    elseif key == Input.KEY_HOME then
        choose(1)
    elseif key == Input.KEY_F1 then
        choose(sel - (LIST_H - 1))
    elseif key == Input.KEY_F7 then
        choose(sel + (LIST_H - 1))
    elseif key == Input.KEY_RETURN or key == Input.KEY_RIGHT then
        open_selected()
    elseif key == Input.KEY_LEFT or key == Input.KEY_BACKSPACE then
        go_up()
    elseif k == 101 then                 -- e
        edit_selected()
    elseif k == 100 then                 -- d
        delete_selected()
    elseif k == 114 then                 -- r
        rename_selected()
    elseif k == 99 then                  -- c
        copy_selected()
    elseif k == 109 then                 -- m
        make_dir()
    elseif key == Input.KEY_ESCAPE or k == 113 then   -- q
        ExitProgram()
    end
end

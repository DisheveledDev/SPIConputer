-- Keys (the OS's on_keypress, key-down only): the search dialog takes
-- every key while it is open.

local function view_key(key)
    if asking then
        search_key(key)
        return
    end
    if key == Input.KEY_ESCAPE or key == 113 or key == 81 then      -- q
        ExitProgram()
    elseif key == Input.KEY_DOWN then
        scroll_to(top + 1)
    elseif key == Input.KEY_UP then
        scroll_to(top - 1)
    elseif key == Input.KEY_SPACE or key == 102 or key == Input.KEY_F7 then  -- f
        scroll_to(top + H - 1)
    elseif key == 98 or key == Input.KEY_F1 then                     -- b
        scroll_to(top - (H - 1))
    elseif key == Input.KEY_HOME or key == 103 then                  -- g
        scroll_to(1)
    elseif key == 71 then                                            -- G
        scroll_to(max_top())
    elseif key == Input.KEY_RIGHT then
        hscroll = hscroll + 8
        draw_page()
    elseif key == Input.KEY_LEFT then
        if hscroll > 0 then
            hscroll = math.max(0, hscroll - 8)
            draw_page()
        end
    elseif key == 47 then                                            -- /
        open_search()
    elseif key == 110 or key == 78 then                              -- n
        if needle == "" then
            open_search()
        else
            find_next(match_line or (top - 1))
        end
    end
end

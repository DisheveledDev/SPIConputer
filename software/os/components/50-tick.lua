-- Main loop: finish after a program returns, route keys to the pager,
-- or the command line, and feed script lines.

local next_blink = 0

local function pager_key(key)
    if key == KEY_ESCAPE or key == KEY_RUNSTOP or key == 113 or key == 81 then
        discard_output()
        if #script == 0 then out("READY.") end
    elseif key == KEY_RETURN then
        page_count = ROWS - 2 -- one more line
    else
        page_count = 0        -- another page
    end
    more_waiting = false
    if flush() then resume() end
end

local function key_pressed(key)
    if more_waiting then
        pager_key(key)
    elseif cursor_col then
        if key == KEY_RETURN then
            local line = input
            remember(line)
            history_pos = 0
            submit(line)
        elseif edit_key(key) then
            cursor_on = true
            draw_input()
        end
    elseif key == KEY_RUNSTOP and #script > 0 then
        script = {}
        out("BREAK")
    end
end

function tick()
    -- First tick after a program exits: its result, then READY.
    if waiting_child then
        waiting_child = false
        local ok, result = UtilityPoll()
        if ok ~= nil then show_utility_result(ok, result) end
        -- The result may have started another program (APPS).
        if not waiting_child then finish_command() end
        return
    end

    while true do
        local event = InputPoll()
        if not event then break end
        if event.type == "key" and event.pressed == 1 then
            key_pressed(event.key)
            -- A program started: the rest of the keys wait for the shell.
            if waiting_child then return end
        end
    end

    if script_ready and not more_waiting then
        script_ready = false
        local line = table.remove(script, 1)
        input, view = line, 0
        submit(line)
    end

    -- The cursor blinks from here rather than from a timer: one
    -- attribute op every half second, nothing in between.
    local now = TimeNow()
    if now >= next_blink then
        next_blink = now + 500
        blink()
    end
end

function finish()
end

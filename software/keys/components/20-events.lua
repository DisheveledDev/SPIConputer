-- Events: every key through Input.Keyboard.Callback("any"), both ports'
-- actions through Input.Joystick.Callback(port, "any").

local function on_key(key, shift, ctrl, cbm, restore)
    log_key(key, shift, ctrl, cbm, restore)
    if key == Input.KEY_RUNSTOP then
        ExitProgram()
    elseif key == Input.KEY_ESCAPE then
        if escape_armed then
            ExitProgram()
            return
        end
        escape_armed = true
        draw_status("ESC again to quit")
    elseif escape_armed then
        escape_armed = false
        draw_status("")
    end
end

local function on_joystick(port, action, down)
    if ACTIONS[action] then draw_action(port, action, down) end
    draw_status(string.format("joystick %d %s %s", port, action, down and "down" or "up"))
end

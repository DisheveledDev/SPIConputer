-- Input: held keys from the raw event stream (presses and releases),
-- joystick 1 read each frame, and the one-shot actions.

local function action()
    if state == "serve" then
        serve()
    elseif state == "clear" then
        level = level + 1
        Overlay.Clear()
        new_level()
    elseif state == "over" then
        score, lives, level = 0, 3, 1
        Overlay.Clear()
        new_level()
    end
end

local function key_event(key, pressed)
    -- A press moves a cell at once (a tap is a step); holding keeps going.
    if key == Input.KEY_LEFT or key == 122 then          -- left / z
        key_left = pressed
        if pressed then nudge = nudge - 1 end
    elseif key == Input.KEY_RIGHT or key == 120 then     -- right / x
        key_right = pressed
        if pressed then nudge = nudge + 1 end
    elseif not pressed then
        return
    elseif key == Input.KEY_SPACE then
        action()
    elseif key == Input.KEY_RETURN and state == "over" then
        action()
    elseif key == Input.KEY_ESCAPE then
        ExitProgram()
    end
end

-- Merge the keyboard and joystick 1 into held_left/held_right; fire is
-- edge-triggered so holding it serves once.
local function read_input()
    while true do
        local ev = Input.Poll()
        if not ev then break end
        if ev.type == "key" then
            key_event(ev.key, ev.pressed == 1)
        end
    end
    local joy = Input.Joystick.State(1) or {}
    held_left = key_left or joy.left or false
    held_right = key_right or joy.right or false
    if joy.fire and not fire_latched then
        action()
    end
    fire_latched = joy.fire or false
end

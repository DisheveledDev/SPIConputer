-- Input: keyboard and joystick 1 both come from the event stream
-- (presses and releases), so nothing is polled or allocated per frame
-- when no input arrives.

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
    elseif key == 109 or key == 77 then -- M
        toggle_music()
    elseif key == Input.KEY_ESCAPE then
        ExitProgram()
    end
end

-- A joystick event carries the directions that changed (INPUT_DIR_*
-- bits: 4 left, 8 right, 16 fire) and whether they went down or up.
local function joystick_event(dirs, pressed)
    if dirs & 4 ~= 0 then joy_left = pressed end
    if dirs & 8 ~= 0 then joy_right = pressed end
    if dirs & 16 ~= 0 and pressed then action() end
end

local function read_input()
    while true do
        local ev = Input.Poll()
        if not ev then break end
        if ev.type == "key" then
            key_event(ev.key, ev.pressed == 1)
        elseif ev.type == "control1" then
            joystick_event(ev.dirs, ev.pressed == 1)
        end
    end
    held_left = key_left or joy_left
    held_right = key_right or joy_right
end

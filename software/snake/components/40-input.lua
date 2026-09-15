-- Input: keyboard and joystick 1 from the event stream, so nothing is
-- polled or allocated when no input arrives.

-- Queue a turn. It is compared with the last queued direction (or the
-- current one), so a reversal or a repeat is dropped and two quick
-- turns between steps both happen.
local function turn(d)
    local last = queued > 0 and queue[queued] or dir
    if d == last or d == OPPOSITE[last] or queued >= #queue then return end
    queued = queued + 1
    queue[queued] = d
end

-- SPACE / fire: start the level, carry on, or play again.
local function action()
    if state == "serve" then
        start()
    elseif state == "paused" then
        resume()
    elseif state == "over" then
        restart()
    end
end

local KEY_DIRS = {
    [Input.KEY_UP] = 1, [Input.KEY_DOWN] = 2, [Input.KEY_LEFT] = 3, [Input.KEY_RIGHT] = 4,
    [119] = 1, [115] = 2, [97] = 3, [100] = 4,      -- w s a d
    [87] = 1, [83] = 2, [65] = 3, [68] = 4,         -- W S A D
}

local function key_pressed(key)
    if key == Input.KEY_ESCAPE then
        ExitProgram()
    elseif state == "play" then
        local d = KEY_DIRS[key]
        if d then
            turn(d)
        elseif key == 112 or key == 80 then          -- p
            pause()
        end
    elseif state == "paused" then
        if key == 112 or key == 80 or key == Input.KEY_SPACE then resume() end
    elseif state == "serve" then
        if key == Input.KEY_SPACE then start() end
    elseif state == "over" then
        if key == Input.KEY_RETURN or key == Input.KEY_SPACE then restart() end
    end
end

-- A joystick event carries the directions that changed (1 up, 2 down,
-- 4 left, 8 right, 16 fire) and whether they went down.
local function joystick_pressed(dirs)
    if state == "play" then
        if dirs & 1 ~= 0 then turn(1) end
        if dirs & 2 ~= 0 then turn(2) end
        if dirs & 4 ~= 0 then turn(3) end
        if dirs & 8 ~= 0 then turn(4) end
    end
    if dirs & 16 ~= 0 then action() end
end

local function read_input()
    while true do
        local ev = Input.Poll()
        if not ev then break end
        if ev.pressed == 1 then
            if ev.type == "key" then
                key_pressed(ev.key)
            elseif ev.type == "control1" then
                joystick_pressed(ev.dirs)
            end
        end
    end
end

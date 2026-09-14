-- SDK: Input
-- Summary: Keyboard and joystick callbacks by key or action, plus key-code names.
-- Namespaces: Input, Input.Keyboard, Input.Joystick
--
-- Same format contract as screen.lua (preamble, `function Name.Sub`
-- blocks stripped when unused, `---` signature lines).
--
-- Callbacks install themselves into the program's on_keypress /
-- on_control hooks the first time one is registered, chaining to any
-- hook the program defined itself. Register them in setup().

Input = Input or {}
Input.Keyboard = Input.Keyboard or {}
Input.Joystick = Input.Joystick or {}

-- Key codes (see lua.md): ASCII for printable keys, these for the rest.
Input.KEY_RETURN = 13
Input.KEY_ESCAPE = 27
Input.KEY_BACKSPACE = 8
Input.KEY_DELETE = 127
Input.KEY_TAB = 9
Input.KEY_SPACE = 32
Input.KEY_UP = 128
Input.KEY_DOWN = 129
Input.KEY_LEFT = 130
Input.KEY_RIGHT = 131
Input.KEY_F1 = 132
Input.KEY_F2 = 133
Input.KEY_F3 = 134
Input.KEY_F4 = 135
Input.KEY_F5 = 136
Input.KEY_F6 = 137
Input.KEY_F7 = 138
Input.KEY_HOME = 139
Input.KEY_RUNSTOP = 140

local key_handlers = {}     -- key code -> fn
local key_any_handler = nil
local keyboard_installed = false

local joy_handlers = {}     -- "port:action" -> fn
local joy_state = { 0, 0 }  -- last seen direction bits per port (1-based)
local joystick_installed = false
local JOY_ACTIONS = { "up", "down", "left", "right", "fire" }

-- A key argument as a code: numbers pass through, one-character strings
-- become their byte, "any" stays a word.
local function key_code(key)
    if type(key) == "string" then
        if key == "any" then return "any" end
        return key:byte(1)
    end
    return key
end

local function install_keyboard()
    if keyboard_installed then return end
    keyboard_installed = true
    local previous = on_keypress
    on_keypress = function(key, shift, ctrl, cbm, restore)
        local fn = key_handlers[key]
        if fn then fn(key, shift, ctrl, cbm, restore) end
        if key_any_handler then key_any_handler(key, shift, ctrl, cbm, restore) end
        if previous then previous(key, shift, ctrl, cbm, restore) end
    end
end

local function install_joystick()
    if joystick_installed then return end
    joystick_installed = true
    local previous = on_control
    on_control = function(index, up, down, left, right, fire)
        local port = index + 1
        local now = { up = up, down = down, left = left, right = right, fire = fire }
        local was = joy_state[port] or {}
        for _, action in ipairs(JOY_ACTIONS) do
            if now[action] ~= (was[action] or false) then
                local fn = joy_handlers[port .. ":" .. action]
                if fn then fn(port, action, now[action]) end
                local any = joy_handlers[port .. ":any"]
                if any then any(port, action, now[action]) end
            end
        end
        joy_state[port] = now
        if previous then previous(index, up, down, left, right, fire) end
    end
end

--- Input.Keyboard.Callback(key, fn)
-- Calls fn(key, shift, ctrl, cbm, restore) when `key` is pressed: a key
-- code, a one-character string such as "a", or "any" for every key.
function Input.Keyboard.Callback(key, fn)
    install_keyboard()
    local code = key_code(key)
    if code == "any" then
        key_any_handler = fn
    else
        key_handlers[code] = fn
    end
    return true
end

--- Input.Keyboard.Remove(key)
-- Removes the callback registered for `key` (or "any").
function Input.Keyboard.Remove(key)
    local code = key_code(key)
    if code == "any" then
        key_any_handler = nil
    else
        key_handlers[code] = nil
    end
    return true
end

--- Input.Joystick.Callback(port, action, fn)
-- Calls fn(port, action, pressed) when joystick `port` (1 or 2) changes
-- `action`: "up", "down", "left", "right", "fire" or "any".
function Input.Joystick.Callback(port, action, fn)
    install_joystick()
    joy_handlers[port .. ":" .. action] = fn
    return true
end

--- Input.Joystick.Remove(port, action)
-- Removes the callback for a port and action.
function Input.Joystick.Remove(port, action)
    joy_handlers[port .. ":" .. action] = nil
    return true
end

--- Input.Joystick.State(port)
-- The current state of joystick `port`: a table with up, down, left,
-- right and fire booleans (nil for an invalid port).
function Input.Joystick.State(port)
    return InputControl(port)
end

--- Input.Poll()
-- The next raw input event (a table with type, key, mods, pressed,
-- ctrl, dirs), or nil when none is waiting.
function Input.Poll()
    return InputPoll()
end

--- Input.Keyboard.Name(key)
-- A readable name for a key code: "RETURN", "F1", "a" and so on.
function Input.Keyboard.Name(key)
    local names = {
        [13] = "RETURN", [27] = "ESCAPE", [8] = "BACKSPACE", [127] = "DELETE",
        [9] = "TAB", [32] = "SPACE", [128] = "UP", [129] = "DOWN",
        [130] = "LEFT", [131] = "RIGHT", [132] = "F1", [133] = "F2",
        [134] = "F3", [135] = "F4", [136] = "F5", [137] = "F6", [138] = "F7",
        [139] = "HOME", [140] = "RUN/STOP",
    }
    if names[key] then return names[key] end
    if key and key >= 33 and key < 127 then return string.char(key) end
    return tostring(key)
end

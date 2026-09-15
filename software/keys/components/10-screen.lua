-- Drawing.

local function draw_status(text)
    Screen.Label(0, STATUS_ROW, 40, " " .. text, "left", HEAD_ATTR)
end

local function draw_action(port, action, on)
    local a = ACTIONS[action]
    local attr = Attributes.Normal
    if on then attr = action == "fire" and FIRE_ATTR or LIT_ATTR end
    Screen.OutText(PANEL_X[port] + a[1], PANEL_TOP + a[2], a[3], attr)
end

local function draw_panels()
    for port = 1, 2 do
        local x = PANEL_X[port]
        Screen.Window(x, PANEL_TOP, x + 17, PANEL_TOP + 8, "JOYSTICK " .. port, Screen.SINGLE)
        local state = Input.Joystick.State(port) or {}
        for action in pairs(ACTIONS) do
            draw_action(port, action, state[action])
        end
    end
end

local function draw_all()
    Screen.Label(0, 0, 40, " KEYS - KEYBOARD AND JOYSTICK TESTER", "left", TITLE_ATTR)
    Screen.OutText(1, LOG_TOP - 1, "CODE  NAME        MODIFIERS", HEAD_ATTR)
    Screen.Label(1, COUNT_ROW, 38, "keys pressed: 0", "left")
    draw_panels()
    draw_status("press keys; simulator joystick: numpad")
    Screen.Label(0, KEYS_ROW, 40, " ESC twice or RUN/STOP to quit", "left", KEYS_ATTR)
end

local function modifiers(shift, ctrl, cbm, restore)
    local parts = {}
    if shift then parts[#parts + 1] = "SHIFT" end
    if ctrl then parts[#parts + 1] = "CTRL" end
    if cbm then parts[#parts + 1] = "C=" end
    if restore then parts[#parts + 1] = "RESTORE" end
    return table.concat(parts, " ")
end

-- A new line at the top of the log: the old lines scroll down one row
-- and the previous newest goes back to plain.
local function log_key(key, shift, ctrl, cbm, restore)
    pressed = pressed + 1
    Screen.Scroll(0, LOG_TOP, 39, LOG_TOP + LOG_ROWS - 1, 0, 1)
    if pressed > 1 then
        Screen.FillAttr(0, LOG_TOP + 1, 39, LOG_TOP + 1, Attributes.Normal)
    end
    Screen.Label(0, LOG_TOP, 40, string.format(" %3d  %-11s %s", key,
                 Input.Keyboard.Name(key):sub(1, 11), modifiers(shift, ctrl, cbm, restore)),
                 "left", NEW_ATTR)
    Screen.Label(1, COUNT_ROW, 38, "keys pressed: " .. pressed, "left")
end

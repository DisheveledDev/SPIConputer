-- keys.lua — SPIComputer keyboard and joystick tester.
--
-- Launch from the shell:  keys   (or pick KEYS in APPS).
--
-- Every key-down is logged with its code, name (Input.Keyboard.Name)
-- and the modifiers held (SHIFT, CTRL, C=, RESTORE), newest at the top:
-- one Screen.Scroll plus one row per key. Below the log, a panel per
-- joystick port lights each direction and fire while it is held, from
-- the joystick events alone (one row op per change). Useful for
-- bringing up the keyboard matrix and joystick wiring on a new board.
--
-- ESC is a key like any other here: press it twice in a row to quit,
-- or press RUN/STOP once.
--
-- Built from the SPIEdit project in this folder: `swift run
-- --package-path ide/macos spibuild software/keys`, or
-- software/install.sh to put it on the card image as apps/keys.app.

local LOG_TOP, LOG_ROWS = 3, 12
local COUNT_ROW = LOG_TOP + LOG_ROWS
local PANEL_TOP = 17
local STATUS_ROW, KEYS_ROW = 27, 29
local TITLE_ATTR = Attributes.Cyan + Attributes.Inverse
local HEAD_ATTR = Attributes.Yellow
local NEW_ATTR = Attributes.Yellow
local KEYS_ATTR = Attributes.White + Attributes.Inverse
local LIT_ATTR = Attributes.Green + Attributes.Inverse
local FIRE_ATTR = Attributes.Red + Attributes.Inverse

-- Where each joystick action is drawn inside a panel (offset from the
-- panel's left edge and top) and its label.
local ACTIONS = {
    up = { 7, 2, " UP " }, down = { 7, 6, "DOWN" },
    left = { 2, 4, "LEFT" }, right = { 12, 4, "RGHT" }, fire = { 7, 4, "FIRE" },
}
local PANEL_X = { 1, 21 }         -- left edge of port 1 and port 2

local pressed = 0
local escape_armed = false

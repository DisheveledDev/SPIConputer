-- editor.lua — SPIComputer OS text editor.
--
-- Launch from the shell:  editor <file>   (or: run editor <file>).
-- The file name arrives as args[1] (and as the chunk's vararg).
--
-- The file is edited entirely in RAM (array of lines) and written back
-- on save, so insertion is fast and the SD card only sees whole-file
-- writes. Files are capped at 128 KB by fs.readall.
--
-- Layout: row 0 is the menu bar, rows 1-28 the text, row 29 the status
-- bar, all on the base layer through the Screen framework. Drop-down
-- menus and dialogs live on the overlay (Overlay.Window / Overlay.Dialog),
-- so opening and closing them never redraws the text underneath. The
-- cursor is a blinking attribute on its cell, driven by a Timer that is
-- paused while a menu or dialog is up.
--
-- Keys:
--   cursor keys   move;  Home  start of line
--   F1/F2/F3/F4   help / FILE / EDIT / OPTIONS menus
--   Ctrl+H/F/E/O  the same
--   Return        split the line
--   Backspace     delete before cursor;  Shift+Backspace inserts a space
--   Forward del   delete at cursor
--   Ctrl+S        save;  Ctrl+Q  quit (asks when the buffer is dirty)
--   printable     insert (shift is already applied by the OS)
--
-- Built from the SPIEdit project in this folder: edit the components
-- here and Build, or run `swift run --package-path ide/macos spibuild
-- software/editor` from the workspace root.

local filename = ... or "untitled.txt"
if filename:sub(1, 6) ~= "/data/" then
    filename = "/data/" .. filename:gsub("^/+", "")
end

local COLS = Screen.COLS
local TEXT_TOP = 1               -- first text row (row 0 is the menu bar)
local H = Screen.ROWS - 2        -- text rows (28); the status bar is the last row
local STATUS_ROW = Screen.ROWS - 1
local INVERT = Screen.INVERT

local lines = {}
local cx, cy = 0, 1              -- cursor: column (0-based), line (1-based)
local scroll_y = 0               -- top visible line (0-based index)
local dirty = false
local cursor_visible = true      -- blink phase
local blink_enabled = true       -- OPTIONS > Toggle cursor blink
local blink_timer = nil

-- What the overlay is showing: nil, "menu" or a dialog kind.
local overlay_mode = nil
local menu_top = 1               -- open menu (index into menu_defs)
local menu_item = 1
local dialog_text = ""           -- input for the go-to-line dialog

local menu_defs = {
    { name = "FILE", items = { "Save", "Go to line", "Quit" } },
    { name = "EDIT", items = { "Top of file", "Bottom of file", "Delete line" } },
    { name = "OPTIONS", items = { "Toggle cursor blink", "File info" } },
    { name = "HELP", items = { "Keyboard help" } },
}

local KEY_RETURN, KEY_ESCAPE, KEY_BACKSPACE, KEY_DELETE = 13, 27, 8, 127
local KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT = 128, 129, 130, 131
local KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_HOME = 132, 133, 134, 135, 139

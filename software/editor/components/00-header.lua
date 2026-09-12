-- editor.lua — SPIComputer OS text editor.
--
-- Launch from the shell:  editor <file>   (or: run editor <file>).
-- The file name arrives as args[1] (and as the chunk's vararg).
--
-- The file is edited entirely in RAM (array of lines) and written back
-- on save, so insertion is fast and the SD card only sees whole-file
-- writes. Files are capped at 128 KB by fs.readall.
--
-- Keys:
--   cursor keys   move
--   F1/F2/F3/F4   help/file/edit/options menus
--   Ctrl+F/E/O/H  open file/edit/options/help menus
--   Return        split the line
--   Backspace     delete before cursor
--   Shift+Del     insert a space (C64 INST semantics)
--   Forward del   delete at cursor
--   Home          start of line
--   Ctrl+S        save
--   Ctrl+Q        quit (asks if the buffer is dirty)
--   printable     insert (shift is already applied by the OS)
--
-- Built from the SPIEdit project in this folder: edit the components
-- here and Build, or run `swift run --package-path ide/macos spibuild
-- software/editor` from the workspace root. The generated editor.lua is
-- written to the parent folder.

local filename = ... or "untitled.txt"
if filename:sub(1, 6) ~= "/data/" then
    filename = "/data/" .. filename:gsub("^/+", "")
end

local W, H = 40, 29        -- text area; screen line 29 is the status bar
local editor_wide = false
local lines = {}
local cx, cy = 0, 1        -- cursor: column (0-based), line (1-based)
local scroll_y = 0         -- top visible line (0-based index)
local dirty = false
local quit_confirm = false
local cursor_on = true
local menu_open = false
local menu_top = 1
local menu_item = 1
local dialog_open = false
local dialog_kind = ""
local dialog_text = ""
local dialog_cursor = 0
local menu_defs = {
    {name = "FILE", items = {"Save", "Go to line", "Quit"}},
    {name = "EDIT", items = {"Top of file", "Bottom of file", "Delete line"}},
    {name = "OPTIONS", items = {"Toggle 40/80", "Toggle cursor", "Clear menu"}},
    {name = "HELP", items = {"Keyboard help"}},
}

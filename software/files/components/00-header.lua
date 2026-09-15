-- files.lua — SPIComputer file manager for the data folder.
--
-- Launch from the shell:  files [folder]   (or pick FILES in APPS).
--
-- Layout: row 0 title bar (path, card space), row 1 column heads, rows
-- 2-27 the listing, row 28 counts, row 29 key help. Folders are cyan and
-- sorted first; the selection is the row's colour inverted. Moving the
-- selection is two attribute ops; stepping past the edge scrolls the
-- list one row (one Screen.Scroll plus one row). Dialogs live on the
-- overlay, so closing one is a single Overlay.Clear.
--
-- Keys:
--   UP / DOWN, HOME     move the selection (F1 / F7 a page)
--   RETURN or RIGHT     open: enter a folder, view a file (apps/view)
--   LEFT or BACKSPACE   up to the parent folder
--   E                   edit the file (apps/editor)
--   C / R / M           copy / rename or move / make a folder (asks a name;
--                       a name with / is relative to data, as in the shell)
--   D                   delete (asks Y/N; folders must be empty)
--   ESC or Q            quit
--
-- Viewing and editing run the other app on top (Execute); the listing
-- is re-read when it returns, since the file may have changed.
--
-- Built from the SPIEdit project in this folder: `swift run
-- --package-path ide/macos spibuild software/files`, or
-- software/install.sh to put it on the card image as apps/files.app.

local COLS <const> = 40
local ROOT <const> = "/data"
local LIST_TOP <const>, LIST_H <const> = 2, 26
local INFO_ROW <const>, KEYS_ROW <const> = 28, 29

local TITLE_ATTR = Attributes.Cyan + Attributes.Inverse
local HEAD_ATTR = Attributes.Yellow
local INFO_ATTR = Attributes.Green
local KEYS_ATTR = Attributes.White + Attributes.Inverse
local DIR_ATTR = Attributes.Cyan
local FILE_ATTR = Attributes.Normal
local UP_ATTR = Attributes.Yellow
local DLG_ATTR = Attributes.White + Attributes.Inverse

-- The listing as two flat arrays (a table per entry costs ~200 bytes of
-- heap on the host; these cost ~50): sizes[i] is FOLDER for a folder and
-- UP for the ".." entry.
local FOLDER <const>, UP <const> = -1, -2
local cwd = ROOT
local names, sizes = {}, {}
local count = 0
local sel, top = 1, 1     -- selected entry, first entry on screen
local child_running = false

-- Dialog state: mode is nil, "message", "confirm" or "input".
local mode = nil
local on_answer = nil     -- confirm: fn(); input: fn(text)
local answer = ""

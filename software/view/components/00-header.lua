-- view.lua — SPIComputer text file viewer.
--
-- Launch from the shell:  view <file>   (or from the FILES app).
--
-- Large files are streamed, never loaded whole: the viewer counts the
-- lines and keeps the start offset of every 16th one (4 bytes each,
-- packed into strings) by reading the file in 4 KB chunks across a few
-- ticks, then reads only the lines on screen, from the nearest
-- checkpoint. A 400 KB file costs about 1.5 KB of index, so big files
-- fit easily in the 96 KB program heap; the count stops at MAX_LINES.
--
-- Layout: row 0 title bar (name, lines shown / total, percent), rows
-- 1-28 the text, row 29 the key help or a message. Long lines are
-- clipped; LEFT/RIGHT scroll sideways. Scrolling one line is one
-- Screen.Scroll plus one line write; a page is one write per row.
--
-- Keys:
--   UP / DOWN          one line
--   SPACE or F / B     page down / up   (F7 / F1 too)
--   HOME or G / shift+G  first line / last line
--   LEFT / RIGHT       scroll sideways 8 columns
--   /                  find (case-insensitive); N next match
--   ESC or Q           quit
--
-- Built from the SPIEdit project in this folder: `swift run
-- --package-path ide/macos spibuild software/view`, or
-- software/install.sh to put it on the card image as apps/view.app.

local COLS = Screen.COLS
local TOP = 1                     -- first text row
local H = Screen.ROWS - 2         -- text rows (28)
local STATUS_ROW = Screen.ROWS - 1
local CHUNK = 4096                -- bytes per read (the fs staging size)
local STRIDE = 16                 -- lines per index checkpoint
local MAX_LINES = 262144          -- line count cap (64 KB of checkpoints)
local TAB = "    "

local TITLE_ATTR = Attributes.Cyan + Attributes.Inverse
local STATUS_ATTR = Attributes.White + Attributes.Inverse
local MATCH_ATTR = Attributes.Yellow + Attributes.Inverse
local HELP = "SPC/B PAGE  G ENDS  / FIND  N NEXT  Q"

local path = args[1] or ...
local file = nil                  -- open handle for the whole session
local size = 0

-- The index: checkpoint j (1-based) is the start offset of line
-- (j - 1) * STRIDE + 1, packed 4 bytes each, 64 to a string.
local blocks = {}                 -- packed strings of 64 "<I4" offsets
local pending = {}                -- checkpoints not yet packed
local npending = 0
local ncp = 0                     -- checkpoints recorded
local started = 0                 -- line starts seen so far
local last_start = 0              -- offset of the latest line start
local index_pos = 0               -- next byte to index
local indexing = true
local truncated = false           -- stopped at MAX_LINES
local lines = 0                   -- complete lines known (final when indexing ends)

-- View state
local top = 1                     -- first line on screen
local hscroll = 0                 -- first column shown
local drawn = false               -- first page drawn
local match_line = nil            -- line highlighted by the last search
local needle = ""                 -- last search text (lower case)

-- The search input dialog (on the overlay)
local asking = false
local answer = ""

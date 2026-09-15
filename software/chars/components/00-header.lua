-- chars.lua — the SPIComputer character set.
--
-- Launch from the shell:  chars   (or pick CHARS in APPS).
--
-- Shows all 256 ROM glyphs as a 16x16 grid (row = high hex digit,
-- column = low digit) inside a frame, with a cursor. Below it: the
-- selected code in decimal, hex and binary, its name (box drawing and
-- symbols follow CP437), and the glyph in each attribute colour, plain
-- and inverted, for picking Attributes values. Each grid row is one
-- display op; moving the cursor is two attribute ops plus the info.
--
-- Keys: cursor keys move, HOME goes to code 0, RETURN/SPACE jump a row
-- (16 codes), ESC or Q quits.
--
-- Built from the SPIEdit project in this folder: `swift run
-- --package-path ide/macos spibuild software/chars`, or
-- software/install.sh to put it on the card image as apps/chars.app.

local GRID_X, GRID_Y = 6, 3       -- cell of code 0
local INFO_Y = 22
local TITLE_ATTR = Attributes.Cyan + Attributes.Inverse
local LABEL_ATTR = Attributes.Yellow
local CURSOR_ATTR = Attributes.Yellow + Attributes.Inverse
local KEYS_ATTR = Attributes.White + Attributes.Inverse
local COLOURS = {
    { "White", Attributes.White }, { "Red", Attributes.Red },
    { "Cyan", Attributes.Cyan }, { "Purple", Attributes.Purple },
    { "Green", Attributes.Green }, { "Blue", Attributes.Blue },
    { "Yellow", Attributes.Yellow }, { "Orange", Attributes.Orange },
}

local code = 65                   -- selected code

-- os.lua — the SPIOS shell (the command line).
--
-- Started by boot at power-on; it is the OS as far as the user sees it,
-- so it has no exit command (RESET restarts the machine). Type a command
-- after READY.; HELP lists the built-ins, UTILS the installed commands.
--
-- Built-ins cover the data folder (DIR, CD, COPY, TYPE, DEL, ...), the
-- screen (CLS, MODE, COLOR), scripts (EXEC, and /data/autoexec.txt at
-- start-up) and the machine (FREE, MEM, UPTIME, VER, RESET). Anything
-- else is a program on the card, found by name in this order:
--   utils/<name>.util   utilities: run once, print their result
--   apps/<name>.app     applications: run on top of the shell
--   games/<name>.game   games: replace the shell; the device restarts
--   loose .prg/.lua     in /apps or /data
--
-- Command line: cursor keys edit, UP/DOWN recall history, TAB completes
-- command and file names (twice lists the choices), ESC clears the line.
-- "Double quotes" keep spaces in one word. `> file` or `>> file` after a
-- built-in or utility writes its output to a file instead of the screen.
-- Output longer than a screen stops at -- MORE -- (SPACE a page, RETURN a
-- line, ESC the rest).
--
-- Arguments reach programs as the global `args`: words that name a data
-- entry or look like a file name become full card paths (/data/...);
-- everything else (numbers, options, quoted words) is passed as typed.
--
-- Drawing is incremental: a finished line is one display op (plus one
-- scroll op once the screen is full), an edit redraws only the input
-- row, and the cursor is one attribute op per blink. Programs keep their
-- own screen slot, so nothing is repainted when one returns.
--
-- Memory: the shell stays resident under every program, inside the
-- 96 KB per-program heap, so rarely used commands live on the card as
-- utilities instead (HELP and UTILS run utils/help.util, COMPILE runs
-- utils/compile.util) and the cursor blinks from tick() without the
-- Timer framework.
--
-- Built from the SPIEdit project in this folder: edit the components
-- here and Build, or run `swift run --package-path ide/macos spibuild
-- software/os` from the workspace root.

local COLS, ROWS = Screen.COLS, Screen.ROWS
-- Key codes (the values of Input.KEY_*; the shell keeps them as locals
-- rather than carry the Input framework's preamble in its heap).
local KEY_RETURN, KEY_ESCAPE, KEY_BACKSPACE, KEY_DELETE, KEY_TAB = 13, 27, 8, 127, 9
local KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_RUNSTOP = 128, 129, 130, 131, 139, 140
local VERSION = "1.1"
local INVERSE = Attributes.Inverse

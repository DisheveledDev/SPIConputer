-- os.lua — the SPIOS shell (the command line).
--
-- Started by boot.lua at power-on. It owns the screen, echoes what you
-- type and runs programs in the foreground: the shell pauses (and its
-- screen state is saved by the OS) until the program exits.
--
-- Commands (HELP lists them too):
--   HELP                 this list
--   DIR                  list files in data
--   APPS                 list installed applications
--   RUN <prog> [args]    run a program from apps or data
--   <prog> [args]        run a program by name
--
-- Everything else on the card is just a program: `editor notes.txt` or
-- `run editor notes.txt` both start the editor. There is no QUIT: the
-- shell is the OS, so it stays running once booted.
--
-- Arguments after a program name arrive in the program as the global
-- `args` table (args[1], args[2], ...). Typing a bare name such as
-- EDITOR finds an installed app or data program whatever its case.
--
-- Built from the SPIEdit project in this folder: edit the components
-- here and Build, or run `swift run --package-path ide/macos spibuild
-- software/os` from the workspace root. The generated os.lua is written
-- to the parent folder (the SD card the OS boots from).

local COLS, ROWS = 40, 30
local KEY_RETURN = 13
local KEY_BACKSPACE = 8
local KEY_DELETE = 127
local KEY_LEFT = 130
local KEY_RIGHT = 131
local KEY_HOME = 139

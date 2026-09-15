-- Commands: built-ins plus program lookup on the card. Anything that
-- is not a built-in is looked up case-insensitively as <name>.prg or
-- <name>.lua and run in the foreground with the remaining words as its
-- `args`.

local cwd = "/"

-- Words separated by spaces. "Double quotes" keep spaces inside a word
-- and mark it as quoted: quoted words reach programs exactly as typed.
local function split_words(text)
    local words, quoted = {}, {}
    local pos = 1
    while true do
        local s = text:find("%S", pos)
        if not s then break end
        if text:sub(s, s) == '"' then
            local e = text:find('"', s + 1, true) or #text + 1
            words[#words + 1] = text:sub(s + 1, e - 1)
            quoted[#words] = true
            pos = e + 1
        else
            local e = text:find("%s", s) or #text + 1
            words[#words + 1] = text:sub(s, e - 1)
            pos = e
        end
    end
    return words, quoted
end

local function tail(words, quoted, first)
    local args, flags = {}, {}
    for i = first, #words do
        args[#args + 1] = words[i]
        flags[#args] = quoted[i]
    end
    return args, flags
end

local function normalize(path)
    local parts = {}
    for part in path:gmatch("[^/]+") do
        if part == ".." then
            if #parts > 0 then
                table.remove(parts)
            end
        elseif part ~= "." then
            parts[#parts + 1] = part
        end
    end
    if #parts == 0 then
        return "/"
    end
    return "/" .. table.concat(parts, "/")
end

local function full_path(path)
    local logical = cwd
    if path and path ~= "" then
        logical = path:sub(1, 1) == "/" and normalize(path)
            or normalize(cwd .. "/" .. path)
    end
    return logical == "/" and "/data" or "/data" .. logical
end

local function parent_path(path)
    local slash = path:match("^.*()/")
    if not slash or slash == 1 then
        return "/"
    end
    return path:sub(1, slash - 1)
end

local function display_path(path)
    if path == "/" then
        return "/"
    end
    return path:sub(2)
end

local function find_app_program(root, entry)
    local folder = root .. "/" .. entry
    local info = fs.stat(folder)
    if not info or not info.dir then
        return nil
    end
    local compiled = fs.find("app.prg", folder)
    if compiled then return folder .. "/" .. compiled end
    local source = fs.find("app.lua", folder)
    if source then return folder .. "/" .. source end
    return nil
end

-- A bundle's program: /<root>/<name><ext>/app.prg (or app.lua).
local function find_bundle(root, name, ext)
    local found = fs.find(name .. ext, root)
    if not found then return nil end
    return find_app_program(root, found)
end

-- A loose program file in /apps or /data.
local function find_program_file(name)
    for _, root in ipairs({ "/apps", "/data" }) do
        local found = fs.find(name, root)
        if found then
            local info = fs.stat(root .. "/" .. found)
            if info and not info.dir then return root .. "/" .. found end
        end
    end
    return nil
end

-- What a command name runs, and how. Utilities (/utils/<name>.util) come
-- first, so they extend the command set like OS commands; then installed
-- apps (/apps/<name>.app), games (/games/<name>.game) and loose .prg or
-- .lua files in /apps or /data. Returns the program path and its kind:
-- "utility", "application", "game" or "program".
local function find_program(name)
    local lower = name:lower()
    if lower:match("%.lua$") or lower:match("%.prg$") then
        local path = find_program_file(name)
        return path, path and "program" or nil
    end
    local util = find_bundle("/utils", name, ".util")
    if util then return util, "utility" end
    local app = find_bundle("/apps", name, ".app")
    if app then return app, "application" end
    local game = find_bundle("/games", name, ".game")
    if game then return game, "game" end
    for _, candidate in ipairs({ name .. ".prg", name .. ".lua" }) do
        local path = find_program_file(candidate)
        if path then return path, "program" end
    end
    return nil
end

-- An argument as the program should see it. Words that name a card
-- entry relative to the current directory, or look like a file name
-- (only name characters, with a letter, and a slash or an extension:
-- notes.txt, games/x), become full card paths ("/data/..."); real card
-- paths that exist, options ("-n"), numbers ("2.5", "1/3"), expressions,
-- patterns and quoted words are passed exactly as typed.
local function path_like(word)
    return word:match("^[%w_%-%./]+$") and word:find("%a")
        and (word:find("/", 1, true) or word:match("%.%a%w*$"))
end

local function program_arg(word, quoted)
    if quoted or word:sub(1, 1) == "-" then return word end
    if word:sub(1, 1) == "/" and fs.exists(word) then return word end
    local path = full_path(word)
    if fs.exists(path) or path_like(word) then
        return path
    end
    return word
end

-- Run a program found by find_program. A game takes the machine over:
-- the shell's state is released (Launch with replace) and the OS
-- restarts when the game exits. Everything else runs on top of the
-- shell and returns to it.
local function run_program(path, args, kind, quoted)
    quoted = quoted or {}
    for i = 1, #args do
        args[i] = program_arg(args[i], quoted[i])
    end
    if kind == "game" then
        local ok, err = Launch(path, args[1], true)
        if not ok then
            out("?" .. tostring(err))
        else
            -- The shell has been replaced: nothing more to print.
            needs_repaint = true
        end
        return
    end
    local ok, err = Execute(path, table.unpack(args))
    if not ok then
        out("?" .. tostring(err))
        return
    end
    needs_repaint = true
end

local function command_error(err)
    out("?" .. tostring(err))
end

local function cmd_help()
    out("COMMANDS:")
    out("  HELP              THIS LIST")
    out("  DIR [PATH]        LIST DATA FILES")
    out("  APPS              CHOOSE AN APP TO RUN")
    out("  CD [PATH]         CHANGE DATA DIRECTORY")
    out("  PWD               SHOW DATA DIRECTORY")
    out("  MD <DIR>          CREATE A DIRECTORY")
    out("  RD <DIR>          REMOVE AN EMPTY DIRECTORY")
    out("  DEL <FILE>        DELETE A FILE")
    out("  REN <OLD> <NEW>   RENAME OR MOVE")
    out("  COPY <FROM> <TO>  COPY A FILE")
    out("  TYPE <FILE>       DISPLAY A TEXT FILE")
    out("  STAT <PATH>       SHOW FILE INFORMATION")
    out("  COMPILE <FILE>    BUILD A .PRG FROM A .LUA")
    out("  FREE              SHOW CARD SPACE")
    out("  MOUNT             REMOUNT THE CARD")
    out("  CLS               CLEAR THE SCREEN")
    out("  MODE [0|1]        CHANGE TEXT MODE")
    out("  ECHO <TEXT>       PRINT TEXT")
    out("  TIME              SHOW MILLISECONDS")
    out("  RUN <PROG> [ARGS] RUN A PROGRAM")
    out("  <PROG> [ARGS]     RUN A PROGRAM BY NAME")
end

local function cmd_dir(path)
    local target = full_path(path)
    local entries, err = fs.ls(target)
    if not entries then
        command_error(err)
        return
    end
    out("DIRECTORY OF DATA/" .. (path or "") .. ":")
    for _, entry in ipairs(entries) do
        out(string.format("  %-22s %7d %s", entry.name, entry.size,
                          entry.dir and "<DIR>" or "     "))
    end
end

-- APPS opens the picker dialog (assigned in 35-apps.lua, which follows
-- this component); the shell's prompt waits until it closes or an app
-- is launched.
local open_apps_dialog

local function cmd_apps()
    open_apps_dialog()
end

local function cmd_cd(path)
    local target = full_path(path)
    local info, err = fs.stat(target)
    if not info then
        command_error(err)
        return
    end
    if not info.dir then
        out("?NOT A DIRECTORY")
        return
    end
    cwd = target
end

local function cmd_mkdir(path)
    if not path then
        out("USAGE: MD <DIRECTORY>")
        return
    end
    local ok, err = fs.mkdir(full_path(path))
    if not ok then command_error(err) end
end

local function cmd_remove(path, directory)
    if not path then
        out(directory and "USAGE: RD <DIRECTORY>" or "USAGE: DEL <FILE>")
        return
    end
    local target = full_path(path)
    local info, err = fs.stat(target)
    if not info then
        command_error(err)
        return
    end
    if directory ~= info.dir then
        out(directory and "?NOT A DIRECTORY" or "?IS A DIRECTORY")
        return
    end
    local ok, remove_error = fs.remove(target)
    if not ok then command_error(remove_error) end
end

local function cmd_rename(old_path, new_path)
    if not old_path or not new_path then
        out("USAGE: REN <OLD> <NEW>")
        return
    end
    local ok, err = fs.rename(full_path(old_path), full_path(new_path))
    if not ok then command_error(err) end
end

local function cmd_copy(source, destination)
    if not source or not destination then
        out("USAGE: COPY <FROM> <TO>")
        return
    end
    local data, err = fs.readall(full_path(source))
    if not data then
        command_error(err)
        return
    end
    local ok, write_error = fs.writeall(full_path(destination), data)
    if not ok then command_error(write_error) end
end

local function cmd_type(path)
    if not path then
        out("USAGE: TYPE <FILE>")
        return
    end
    local data, err = fs.readall(full_path(path))
    if not data then
        command_error(err)
        return
    end
    out(data:gsub("\n$", ""))
end

local function cmd_stat(path)
    if not path then
        out("USAGE: STAT <PATH>")
        return
    end
    local target = full_path(path)
    local info, err = fs.stat(target)
    if not info then
        command_error(err)
        return
    end
    out(string.format("PATH: %s", target))
    out(string.format("TYPE: %s", info.dir and "DIRECTORY" or "FILE"))
    out(string.format("SIZE: %d BYTES", info.size))
end

local function cmd_free()
    local free, total = fs.free()
    if not free then
        out("?CARD SPACE UNAVAILABLE")
        return
    end
    out(string.format("CARD SPACE: %d KB FREE / %d KB TOTAL", free, total))
end

local function cmd_mode(value)
    if not value then
        out(string.format("TEXT MODE: %d (%dx%d)", shell_mode, COLS, ROWS))
        return
    end
    local mode = tonumber(value)
    if mode ~= 0 and mode ~= 1 then
        out("?USE MODE 0 OR 1")
        return
    end
    local ok, err = ScreenMode(mode)
    if not ok then
        command_error(err)
        return
    end
    shell_mode = mode
    if mode == 0 or mode == 1 then
        COLS, ROWS = 40, 30
    else
        COLS, ROWS = 80, 60
    end
    lines = {}
    input = ""
    caret_col = 0
    prompt_row = 0
    paint()
end

-- COMPILE <FILE.LUA> [OUT]: build a .prg next to the source (or at OUT)
-- with the OS's own compiler, so a program written on the card with the
-- editor loads as fast as one built by the IDE. The OS prefers a .prg
-- over its .lua sibling when a program is run by name.
local function cmd_compile(source, destination)
    if not source then
        out("USAGE: COMPILE <FILE.LUA> [OUT.PRG]")
        return
    end
    local src = full_path(source)
    local dst = destination and full_path(destination) or nil
    local ok, result = Compile(src, dst)
    if not ok then
        command_error(result)
        return
    end
    local shown = dst or (src:gsub("%.[Ll][Uu][Aa]$", "") .. ".prg")
    shown = shown:gsub("^/data", "")
    if shown == "" then shown = "/" end
    out(string.format("%s: %d BYTES", display_path(shown), result))
end

local function execute(command_line)
    local words, quoted = split_words(command_line)
    local name = words[1]
    if not name then
        return
    end
    local lower = name:lower()
    if lower == "help" or lower == "?" then
        cmd_help()
    elseif lower == "apps" then
        cmd_apps()
    elseif lower == "dir" or lower == "ls" or lower == "catalog" then
        cmd_dir(words[2])
    elseif lower == "cd" or lower == "chdir" then
        cmd_cd(words[2])
    elseif lower == "pwd" then
        out(cwd)
    elseif lower == "md" or lower == "mkdir" then
        cmd_mkdir(words[2])
    elseif lower == "rd" or lower == "rmdir" then
        cmd_remove(words[2], true)
    elseif lower == "del" or lower == "erase" or lower == "rm" then
        cmd_remove(words[2], false)
    elseif lower == "ren" or lower == "rename" or lower == "move" then
        cmd_rename(words[2], words[3])
    elseif lower == "copy" or lower == "cp" then
        cmd_copy(words[2], words[3])
    elseif lower == "type" or lower == "cat" or lower == "print" then
        cmd_type(words[2])
    elseif lower == "stat" or lower == "info" then
        cmd_stat(words[2])
    elseif lower == "free" then
        cmd_free()
    elseif lower == "compile" then
        cmd_compile(words[2], words[3])
    elseif lower == "mount" then
        if not fs.mount() then out("?MOUNT FAILED") end
    elseif lower == "cls" or lower == "clear" then
        lines = {}
        paint()
    elseif lower == "mode" or lower == "resolution" then
        cmd_mode(words[2])
    elseif lower == "echo" then
        out(command_line:sub(#name + 2))
    elseif lower == "time" then
        out(string.format("%d MS", TimeNow()))
    elseif lower == "run" then
        local program = words[2]
        if not program then
            out("USAGE: RUN <PROGRAM> [ARGS]")
            return
        end
        local path, kind = find_program(program)
        if not path then
            out("?FILE NOT FOUND: " .. program)
            return
        end
        local args, flags = tail(words, quoted, 3)
        run_program(path, args, kind, flags)
    else
        local path, kind = find_program(name)
        if not path then
            out("?FILE NOT FOUND: " .. name)
            return
        end
        local args, flags = tail(words, quoted, 2)
        run_program(path, args, kind, flags)
    end
end

local function submit()
    local command_line = input
    commit(command_line)
    input = ""
    caret_col = 0
    paint()
    execute(command_line)
    if not needs_repaint and not dialog_open then
        out("READY.")
    end
end

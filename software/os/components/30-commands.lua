-- Commands: built-ins plus program lookup on the card. Anything that
-- is not a built-in is looked up case-insensitively as <name>.prg or
-- <name>.lua and run in the foreground with the remaining words as its
-- `args`.

local cwd = "/"

local function tail(words, first)
    local args = {}
    for i = first, #words do
        args[#args + 1] = words[i]
    end
    return args
end

local function split_words(text)
    local words = {}
    for word in text:gmatch("%S+") do
        words[#words + 1] = word
    end
    return words
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

local function find_program(name)
    local roots = { "/apps", "/data" }
    local lower = name:lower()
    local names = { name }
    if not lower:match("%.lua$") and not lower:match("%.prg$") then
        names = { name .. ".prg", name .. ".lua" }
    end
    for _, root in ipairs(roots) do
        for _, candidate in ipairs(names) do
            local found = fs.find(candidate, root)
            if found then
                return root .. "/" .. found
            end
        end
    end
    return nil
end

local function run_program(path, args)
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
    out("  APPS              LIST INSTALLED APPS")
    out("  CD [PATH]         CHANGE DATA DIRECTORY")
    out("  PWD               SHOW DATA DIRECTORY")
    out("  MD <DIR>          CREATE A DIRECTORY")
    out("  RD <DIR>          REMOVE AN EMPTY DIRECTORY")
    out("  DEL <FILE>        DELETE A FILE")
    out("  REN <OLD> <NEW>   RENAME OR MOVE")
    out("  COPY <FROM> <TO>  COPY A FILE")
    out("  TYPE <FILE>       DISPLAY A TEXT FILE")
    out("  STAT <PATH>       SHOW FILE INFORMATION")
    out("  FREE              SHOW CARD SPACE")
    out("  MOUNT             REMOUNT THE CARD")
    out("  CLS               CLEAR THE SCREEN")
    out("  MODE [40|80]      CHANGE TEXT WIDTH")
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

local function cmd_apps()
    local entries, err = fs.ls("/apps")
    if not entries then
        command_error(err)
        return
    end
    out("INSTALLED APPS:")
    for _, entry in ipairs(entries) do
        if entry.dir or not entry.name:lower():match("%.prg$") then
            out(string.format("  %-22s %7d %s", entry.name, entry.size,
                              entry.dir and "<DIR>" or "     "))
        end
    end
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
    if mode == 40 then mode = shell_mode == 3 and 3 or 1 end
    if mode == 80 then mode = shell_mode == 1 and 2 or 3 end
    if mode ~= 0 and mode ~= 1 and mode ~= 2 and mode ~= 3 then
        out("?USE MODE 40, 80, 0, 1, 2 OR 3")
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

local function execute(command_line)
    local words = split_words(command_line)
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
        local path = find_program(program)
        if not path then
            out("?FILE NOT FOUND: " .. program)
            return
        end
        run_program(path, tail(words, 3))
    else
        local path = find_program(name)
        if not path then
            out("?FILE NOT FOUND: " .. name)
            return
        end
        run_program(path, tail(words, 2))
    end
end

local function submit()
    local command_line = input
    commit(command_line)
    input = ""
    caret_col = 0
    paint()
    execute(command_line)
    if not needs_repaint then
        out("READY.")
    end
end

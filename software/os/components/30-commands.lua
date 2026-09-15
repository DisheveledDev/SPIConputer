-- Commands: paths, finding programs on the card, redirection, and the
-- built-in command table (HELP and TAB completion read it too).

local cwd = "/data"        -- real card path of the current data directory
local text_mode = 1
local waiting_child = false -- a program is running on top of the shell
local script = {}           -- queued script lines (EXEC, autoexec)
local redirect_file = nil   -- open file while output is redirected

-- ---------------------------------------------------------------- paths

local function normalize(path)
    local parts = {}
    for part in path:gmatch("[^/]+") do
        if part == ".." then
            if #parts > 0 then table.remove(parts) end
        elseif part ~= "." then
            parts[#parts + 1] = part
        end
    end
    return "/" .. table.concat(parts, "/")
end

-- A path as typed (relative to the current directory, or to the data
-- folder when it starts with "/") to its real card path under /data.
local function full_path(path)
    if not path or path == "" then return cwd end
    local logical
    if path:sub(1, 1) == "/" then
        logical = normalize(path)
    else
        logical = normalize(cwd:sub(6) .. "/" .. path)
    end
    return logical == "/" and "/data" or "/data" .. logical
end

-- A real card path as the user sees it (the data folder is "/").
local function display_path(path)
    local shown = path:gsub("^/data", "")
    return shown == "" and "/" or shown
end

-- DOS-style wildcards (* and ?) to an anchored, case-insensitive pattern.
local function wildcard(pattern)
    local lua = pattern:lower():gsub("[%^%$%(%)%%%.%[%]%+%-]", "%%%0")
    lua = lua:gsub("%*", ".*"):gsub("%?", ".")
    return "^" .. lua .. "$"
end

-- The entries of a directory argument that may end in a wildcard
-- ("*.txt", "notes/a*"): returns the directory, the matching entries
-- sorted (directories first) and the pattern, or nil and an error.
local function matching_entries(arg)
    local dir_arg, pattern = arg, nil
    if arg and arg:find("[*?]") then
        dir_arg, pattern = arg:match("^(.*)/([^/]*)$")
        if not dir_arg then dir_arg, pattern = nil, arg end
    end
    local dir = full_path(dir_arg)
    local entries, err = fs.ls(dir)
    if not entries then return nil, err end
    local list = {}
    local match = pattern and wildcard(pattern)
    for _, entry in ipairs(entries) do
        if not match or entry.name:lower():match(match) then
            list[#list + 1] = entry
        end
    end
    table.sort(list, function(a, b)
        if a.dir ~= b.dir then return a.dir end
        return a.name:lower() < b.name:lower()
    end)
    return dir, list, pattern
end

-- A file's lines on demand (a producer for the pager or a script).
local function file_lines(path)
    local f, err = fs.open(path, "r")
    if not f then return nil, err end
    local buffer, done = "", false
    local producer = {}
    function producer.close()
        if f then f:close() f = nil end
    end
    function producer.next()
        while true do
            local nl = buffer:find("\n", 1, true)
            if nl then
                local line = buffer:sub(1, nl - 1)
                buffer = buffer:sub(nl + 1)
                return (line:gsub("\r$", ""):gsub("\t", "  "))
            end
            if done then
                if buffer == "" then return nil end
                local last = buffer
                buffer = ""
                return last
            end
            local chunk = f:read(512)
            if not chunk or chunk == "" then
                done = true
                producer.close()
            else
                buffer = buffer .. chunk
            end
        end
    end
    return producer
end

-- ------------------------------------------------------------- programs

local function find_app_program(folder)
    local info = fs.stat(folder)
    if not info or not info.dir then return nil end
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
    return find_app_program(root .. "/" .. found)
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

-- What a command name runs, and how: "utility", "application", "game"
-- or "program" (a loose .prg/.lua).
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

-- A word that looks like a file name: only name characters, a letter,
-- and a slash or an extension (notes.txt, games/x; not 2.5 or 1/3).
local function path_like(word)
    return word:match("^[%w_%-%./]+$") and word:find("%a")
        and (word:find("/", 1, true) or word:match("%.%a%w*$"))
end

-- An argument as the program sees it: data entries and file-name-like
-- words become full card paths; real card paths that exist, options,
-- numbers, expressions and quoted words pass as typed.
local function program_arg(word, quoted)
    if quoted or word:sub(1, 1) == "-" then return word end
    if word:sub(1, 1) == "/" and fs.exists(word) then return word end
    local path = full_path(word)
    if fs.exists(path) or path_like(word) then return path end
    return word
end

local function end_redirect()
    sink = nil
    if redirect_file then
        redirect_file:close()
        redirect_file = nil
    end
end

-- Run a program. A game replaces the shell (Launch with replace; the
-- device restarts when it exits); anything else runs on top, and the
-- shell carries on in its first tick after the program exits. Nothing
-- may be drawn after a successful start: the program's screen slot is
-- the one showing.
local function run_program(path, args, kind, quoted)
    quoted = quoted or {}
    local expanded = {}
    for i = 1, #args do
        local word = args[i]
        -- Wildcards expand to the matching entries' paths, as in a Unix
        -- shell (quote a pattern to pass it through: find "*.txt").
        local dir, list = nil, nil
        if not quoted[i] and word:find("[*?]") then
            dir, list = matching_entries(word)
        end
        if dir and #list > 0 then
            for _, entry in ipairs(list) do
                expanded[#expanded + 1] = dir .. "/" .. entry.name
            end
        else
            expanded[#expanded + 1] = program_arg(word, quoted[i])
        end
    end
    args = expanded
    if kind ~= "utility" then end_redirect() end
    local ok, err
    if kind == "game" then
        ok, err = Launch(path, args[1], true)
    else
        ok, err = Execute(path, table.unpack(args))
    end
    if not ok then
        out_error(err)
        return
    end
    waiting_child = true
end

-- ------------------------------------------------------------- built-ins

-- Built-in commands: lower-case name or alias -> function(args, quoted).
-- Their help text lives in the HELP utility (utils/help.util), which
-- keeps it out of the shell's heap.
local commands = {}

local function command(names, fn)
    for _, name in ipairs(names) do commands[name] = fn end
end

-- Run one of the shell's own utilities (HELP, DIR, COMPILE live on the
-- card). The shell has already resolved their arguments, so they pass
-- through as if quoted.
local function run_utility(name, args)
    local path = find_bundle("/utils", name, ".util")
    if not path then
        out_error(name:upper() .. " IS NOT INSTALLED (UTILS/" .. name:upper() .. ".UTIL)")
        return
    end
    local quoted = {}
    for i = 1, #args do quoted[i] = true end
    run_program(path, args, "utility", quoted)
end

local function need(value, usage)
    if value then return true end
    out_error("USAGE: " .. usage)
    return false
end

command({ "help", "?" }, function(a)
    run_utility("help", a)
end)

command({ "utils", "commands" }, function()
    run_utility("help", { "-utils" })
end)

command({ "compile" }, function(a)
    run_utility("compile", a)
end)

-- DIR lists through utils/dir.util: the shell hands it the directory
-- resolved against the current one, so the listing (and a big
-- directory's entry table) is built in the utility's heap, not here.
command({ "dir", "ls", "catalog" }, function(a)
    run_utility("dir", { full_path(a[1]) })
end)

command({ "cd", "chdir" }, function(a)
    local target = full_path(a[1] or "/")
    local info, err = fs.stat(target)
    if not info then out_error(err) return end
    if not info.dir then out_error("NOT A DIRECTORY") return end
    cwd = target
end)

command({ "pwd" }, function()
    out(display_path(cwd))
end)

command({ "type", "cat", "more" }, function(a)
    if not need(a[1], "TYPE <FILE>") then return end
    local producer, err = file_lines(full_path(a[1]))
    if not producer then out_error(err) return end
    emit(producer)
end)

command({ "free", "df" }, function()
    local free, total = fs.free()
    if not free then out_error("CARD SPACE UNAVAILABLE") return end
    out(string.format("%s KB FREE OF %s KB", Text.Commas(free), Text.Commas(total)))
end)

command({ "mount" }, function()
    if not fs.mount() then out_error("MOUNT FAILED") end
end)

command({ "cls", "clear", "home" }, function()
    Screen.Clear()
    row = 0
end)

command({ "mode" }, function(a)
    if not a[1] then
        out("TEXT MODE " .. text_mode .. " (0 B&W, 1 COLOUR)")
        return
    end
    local mode = tonumber(a[1])
    if mode ~= 0 and mode ~= 1 then out_error("USE MODE 0 OR 1") return end
    local ok, err = Screen.Mode(mode)
    if not ok then out_error(err) return end
    text_mode = mode
    row = 0
end)

-- Named colours for COLOR (or six hex digits: COLOR FFFFFF 000080).
local COLOURS = {
    black = 0x000000, white = 0xFFFFFF, red = 0x880000, cyan = 0xAAFFEE,
    purple = 0xCC44CC, green = 0x00CC55, blue = 0x0000AA, yellow = 0xFFFF00,
    orange = 0xDD8855, grey = 0x777777, amber = 0xFFB000,
}

local function colour_value(name)
    if not name then return nil end
    local value = COLOURS[name:lower()]
    if value then return value end
    if name:match("^#?%x%x%x%x%x%x$") then return tonumber((name:gsub("#", "")), 16) end
    return nil
end

local function set_palette(index, rgb)
    Screen.Palette(index, rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF)
end

local function default_colours()
    set_palette(0, 0x0000AA) -- background: CPC blue
    set_palette(1, 0xFFFF00) -- text: CPC yellow
end

command({ "color", "colour" }, function(a)
    if not a[1] then
        default_colours()
        return
    end
    local fg, bg = colour_value(a[1]), colour_value(a[2])
    if not fg or (a[2] and not bg) then
        out_error("COLOURS: BLACK WHITE RED CYAN PURPLE GREEN")
        out("BLUE YELLOW ORANGE GREY AMBER, OR RRGGBB")
        return
    end
    set_palette(1, fg)
    if bg then set_palette(0, bg) end
end)

command({ "echo" }, function(a)
    out(table.concat(a, " "))
end)

command({ "mem" }, function()
    local before = collectgarbage("count")
    collectgarbage("collect")
    out(string.format("SHELL HEAP %.1f KB (%.1f BEFORE GC)",
                      collectgarbage("count"), before))
end)

command({ "history", "h" }, function()
    for i, line in ipairs(history) do
        out(string.format("%3d %s", i, line))
    end
end)

command({ "exec", "source", "batch" }, function(a)
    if not need(a[1], "EXEC <SCRIPT FILE>") then return end
    local producer, err = file_lines(full_path(a[1]))
    if not producer then out_error(err) return end
    local lines = {}
    for line in producer.next do
        local text = line:match("^%s*(.-)%s*$")
        local lower = text:lower()
        if text ~= "" and text:sub(1, 1) ~= "#" and lower ~= "rem"
            and lower:sub(1, 4) ~= "rem " then
            lines[#lines + 1] = text
        end
        if #lines >= 200 then break end
    end
    producer.close()
    for i = #lines, 1, -1 do table.insert(script, 1, lines[i]) end
end)

command({ "run" }, function(a, q)
    if not need(a[1], "RUN <PROGRAM> [ARGS]") then return end
    local path, kind = find_program(a[1])
    if not path then out_error("FILE NOT FOUND: " .. a[1]) return end
    table.remove(a, 1)
    table.remove(q, 1)
    run_program(path, a, kind, q)
end)

-- APPS runs the launcher (core/apps.prg), which ends with the chosen
-- program as its result; the shell then runs it (see 40-main).
command({ "apps" }, function()
    local path = fs.find("apps.prg", "/core") or fs.find("apps.lua", "/core")
    if not path then out_error("CORE/APPS.PRG IS MISSING") return end
    run_program("/core/" .. path, {}, "application")
end)

command({ "reset", "reboot" }, function()
    ExitProgram()
end)

-- Aliases: a first word that names one is replaced by its text. The
-- file commands are utilities on the card (utils/copy.util, ...), so
-- their familiar other names are aliases; ALIAS adds more for the
-- session (autoexec.txt is the place to keep them).
local aliases = {
    cp = "copy", rm = "del", erase = "del", mv = "ren", move = "ren",
    rename = "ren", mkdir = "md", rmdir = "rd", new = "touch", info = "stat",
    ver = "sysinfo", version = "sysinfo", uptime = "sysinfo", time = "sysinfo",
}

command({ "alias" }, function(a)
    if not a[1] then
        local names = {}
        for name in pairs(aliases) do names[#names + 1] = name end
        table.sort(names)
        for _, name in ipairs(names) do
            out(string.format("%-9s %s", name:upper(), aliases[name]))
        end
        return
    end
    local name = a[1]:lower()
    table.remove(a, 1)
    aliases[name] = #a > 0 and table.concat(a, " ") or nil
end)

-- ------------------------------------------------------------- dispatch

-- Words separated by spaces; "double quotes" keep spaces in one word and
-- mark it quoted (passed to programs exactly as typed).
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
            quoted[#words] = false
            pos = e
        end
    end
    return words, quoted
end

-- Take a `> file` / `>> file` (or `>file`) off the words and point the
-- output at that file until the command (or its utility) finishes.
local function take_redirect(words, quoted)
    for i = 2, #words do
        local word = words[i]
        if not quoted[i] and word:sub(1, 1) == ">" then
            local append = word:sub(1, 2) == ">>"
            local name = word:sub(append and 3 or 2)
            local used = 1
            if name == "" then
                name = words[i + 1]
                used = 2
            end
            for _ = 1, used do
                table.remove(words, i)
                table.remove(quoted, i)
            end
            if not name then return nil, "MISSING FILE NAME AFTER >" end
            local f, err = fs.open(full_path(name), append and "a" or "w")
            if not f then return nil, err end
            redirect_file = f
            sink = function(line) f:write(line .. "\n") end
            return true
        end
    end
    return true
end

local function execute(command_line)
    local words, quoted = split_words(command_line)
    if not words[1] then return end
    local ok, err = take_redirect(words, quoted)
    if not ok then out_error(err) return end
    local name = table.remove(words, 1)
    table.remove(quoted, 1)
    local alias = aliases[name:lower()]
    if alias then
        local extra, extra_quoted = split_words(alias)
        name = table.remove(extra, 1)
        table.remove(extra_quoted, 1)
        for i = #extra, 1, -1 do
            table.insert(words, 1, extra[i])
            table.insert(quoted, 1, extra_quoted[i])
        end
    end
    local builtin = commands[name:lower()]
    if builtin then
        builtin(words, quoted)
        return
    end
    local path, kind = find_program(name)
    if not path then
        out_error("FILE NOT FOUND: " .. name)
        return
    end
    run_program(path, words, kind, quoted)
end

-- ----------------------------------------------------------- completion

local last_tab = nil       -- the line at the previous TAB (twice = list)

local function common_prefix(list)
    local prefix = list[1]
    for i = 2, #list do
        local other = list[i]
        local n = 0
        while n < #prefix and n < #other
            and prefix:sub(n + 1, n + 1):lower() == other:sub(n + 1, n + 1):lower() do
            n = n + 1
        end
        prefix = prefix:sub(1, n)
    end
    return prefix
end

-- TAB: complete the word before the caret from the command names and
-- installed programs (first word) or the entries of its directory.
complete = function()
    local before = input:sub(1, caret)
    local start = before:find("[^%s\"]*$")
    local word = before:sub(start)
    local first = not before:sub(1, start - 1):find("%S")
    local dir_part = word:match("^(.*/)") or ""
    local prefix = word:sub(#dir_part + 1):lower()
    local seen, candidates = {}, {}
    local function consider(name, suffix)
        if name:lower():sub(1, #prefix) == prefix and not seen[name] then
            seen[name] = true
            candidates[#candidates + 1] = name .. suffix
        end
    end
    if first and dir_part == "" then
        for name in pairs(commands) do consider(name, " ") end
        for name in pairs(aliases) do consider(name, " ") end
        for _, root in ipairs({ "/utils", "/apps", "/games" }) do
            for _, entry in ipairs(fs.ls(root) or {}) do
                consider((entry.name:lower():gsub("%.%w+$", "")), " ")
            end
        end
    else
        for _, entry in ipairs(fs.ls(full_path(dir_part)) or {}) do
            consider(entry.name, entry.dir and "/" or " ")
        end
    end
    if #candidates == 0 then return end
    table.sort(candidates)
    local replacement
    if #candidates == 1 then
        replacement = candidates[1]
    else
        replacement = common_prefix(candidates)
        if #replacement <= #prefix then
            if last_tab == input then
                -- Second TAB: list the choices above the line.
                commit_input()
                local line = ""
                for _, candidate in ipairs(candidates) do
                    local item = candidate:gsub(" $", "")
                    if #line + #item + 2 > COLS then
                        out(line)
                        line = ""
                    end
                    line = line .. item .. "  "
                end
                out(line)
                page_count = 0
                flush()
            end
            last_tab = input
            return
        end
    end
    input = input:sub(1, start - 1) .. dir_part .. replacement .. input:sub(caret + 1)
    caret = start - 1 + #dir_part + #replacement
    last_tab = input
end

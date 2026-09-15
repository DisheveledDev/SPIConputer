-- Built-in commands: usage, one-line summary, detail, other names.
local BUILTINS = {
    { "HELP [COMMAND]", "THIS LIST", "Lists the commands, or describes one.", "?" },
    { "DIR [PATH|*.EXT]", "LIST FILES", "Lists a directory, directories first, with a total. A pattern with * and ? filters the names.", "LS CATALOG" },
    { "CD [PATH]", "CHANGE DIRECTORY", "Changes the current data directory. CD alone goes to the top; .. goes up.", "CHDIR" },
    { "PWD", "SHOW DIRECTORY", "Prints the current data directory." },
    { "TYPE <FILE>", "SHOW A TEXT FILE", "Prints a text file, a screen at a time for long files (SPACE page, RETURN line, ESC stop).", "CAT MORE" },
    { "EXEC <FILE>", "RUN A SCRIPT", "Runs the commands in a text file, one per line; # or REM lines are comments. /autoexec.txt runs at start-up. RUN/STOP stops a script.", "SOURCE BATCH" },
    { "ALIAS [NAME [TEXT]]", "NAME A COMMAND", "Lists aliases, defines one (ALIAS L DIR /), or removes one (ALIAS L). Put them in autoexec.txt to keep them." },
    { "HISTORY", "RECENT COMMANDS", "Lists the last commands. UP and DOWN recall them on the command line.", "H" },
    { "ECHO <TEXT>", "PRINT TEXT", "Prints its words; with > FILE it writes them to a file." },
    { "CLS", "CLEAR THE SCREEN", "Clears the screen.", "CLEAR HOME" },
    { "MODE [0-3]", "TEXT MODE", "0 and 1 are 40x30, 2 and 3 80x60 (80 columns); even is black and white, odd colour. The screen clears." },
    { "COLOR [FG] [BG]", "SCREEN COLOURS", "Sets the text and background colours: BLACK WHITE RED CYAN PURPLE GREEN BLUE YELLOW ORANGE GREY AMBER, or six hex digits RRGGBB. COLOR alone resets them.", "COLOUR" },
    { "FREE", "CARD SPACE", "Shows free and total space on the SD card.", "DF" },
    { "MEM", "SHELL MEMORY", "Shows the shell's Lua heap after a collection." },
    { "MOUNT", "REMOUNT THE CARD", "Mounts the SD card again after a swap." },
    { "APPS", "CHOOSE AN APP", "Opens the launcher: installed applications and games." },
    { "RUN <PROG> [ARGS]", "RUN A PROGRAM", "Runs a program by name or path. Typing the name alone does the same." },
    { "RESET", "RESTART MACHINE", "Restarts the computer.", "REBOOT" },
}

local FOOTER = {
    "",
    "COPY DEL REN MD RD TOUCH STAT COMPILE",
    "AND MORE ARE UTILITIES: SEE UTILS.",
    "TAB COMPLETES, UP/DOWN RECALLS, ESC",
    "CLEARS. CMD > FILE (>> APPENDS) SAVES",
    "OUTPUT. \"QUOTES\" KEEP SPACES; *.TXT",
    "EXPANDS TO MATCHING FILES.",
}

-- One string field of a bundle's app.json (flat, generated JSON).
local function json_field(text, key)
    return text:match('"' .. key .. '"%s*:%s*"(.-)"')
end

local function installed(root, ext)
    local found = {}
    for _, entry in ipairs(fs.ls(root) or {}) do
        if entry.dir then
            local meta = fs.readall(root .. "/" .. entry.name .. "/app.json") or ""
            found[#found + 1] = {
                name = (json_field(meta, "name") or entry.name:gsub(ext .. "$", "")):upper(),
                description = json_field(meta, "description") or "",
            }
        end
    end
    table.sort(found, function(a, b) return a.name < b.name end)
    return found
end

local function wrap(text, lines, indent)
    for _, line in ipairs(Text.Wrap(text, 40 - #indent)) do
        lines[#lines + 1] = indent .. line
    end
end

local function list_utils(lines)
    local utils = installed("/utils", "%.util")
    if #utils == 0 then
        lines[#lines + 1] = "NO UTILITIES INSTALLED"
    end
    for _, util in ipairs(utils) do
        local wrapped = Text.Wrap(util.description, 31)
        lines[#lines + 1] = string.format("%-8s %s", util.name, wrapped[1] or "")
        for i = 2, #wrapped do
            lines[#lines + 1] = string.rep(" ", 9) .. wrapped[i]
        end
    end
end

function setup()
    local lines = {}
    local topic = args[1] and args[1]:upper()
    if topic == "-UTILS" then
        list_utils(lines)
        UtilityResult(true, { lines = lines })
        return
    end
    if not topic then
        for _, b in ipairs(BUILTINS) do
            lines[#lines + 1] = string.format("%-20s%s", b[1], b[2])
        end
        for _, line in ipairs(FOOTER) do lines[#lines + 1] = line end
        UtilityResult(true, { lines = lines })
        return
    end
    for _, b in ipairs(BUILTINS) do
        local names = " " .. b[1]:match("^(%S+)") .. " " .. (b[4] or "") .. " "
        if names:find(" " .. topic .. " ", 1, true) then
            lines[#lines + 1] = b[1]
            wrap(b[3], lines, "  ")
            if b[4] then lines[#lines + 1] = "  ALSO: " .. b[4] end
            UtilityResult(true, { lines = lines })
            return
        end
    end
    for _, root in ipairs({ { "/utils", "%.util", "UTILITY" }, { "/apps", "%.app", "APPLICATION" },
                            { "/games", "%.game", "GAME" } }) do
        for _, item in ipairs(installed(root[1], root[2])) do
            if item.name == topic then
                lines[#lines + 1] = item.name .. " (" .. root[3] .. ")"
                wrap(item.description, lines, "  ")
                UtilityResult(true, { lines = lines })
                return
            end
        end
    end
    UtilityResult(false, "NO HELP FOR " .. topic)
end

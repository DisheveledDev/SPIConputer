-- Searching: every file is streamed line by line; matches are listed
-- while they fit the result budget and counted regardless.

local CHUNK = 1024
-- Listings stay small: the shell rebuilds the result table in its own
-- heap, which has little room to spare (8 KB is only the hard cap).
local MAX_LINES = 60
local BUDGET = 2500       -- bytes of listing
local LONGEST = 160      -- a matching line is cut to this many characters
local USAGE = "USAGE: GREP [-I] [-N] [-C] TEXT FILE..."

local function card_path(p)
    if p:sub(1, 1) == "/" then return p end
    return "/data/" .. p
end

local function shown(p)
    local s = p:gsub("^/data/?", "")
    if s == "" then return "/" end
    return s
end

-- The listing, kept inside the result budget. Control characters are
-- shown as dots so binary files cannot blow the budget up with escapes.
local listing, listing_bytes, dropped = {}, 0, 0

local function emit(line)
    line = line:gsub("%c", ".")
    if #line > LONGEST then line = line:sub(1, LONGEST - 2) .. ".." end
    if dropped > 0 or #listing >= MAX_LINES or listing_bytes + #line + 8 > BUDGET then
        dropped = dropped + 1
        return
    end
    listing[#listing + 1] = line
    listing_bytes = listing_bytes + #line + 8
end

-- Calls fn(line, number) for each line of an open file. A line longer
-- than 4 KB is handed over in pieces rather than held whole.
local function each_line(f, fn)
    local rest, number = "", 0
    while true do
        local chunk = f:read(CHUNK)
        if not chunk or chunk == "" then break end
        rest = rest .. chunk
        local start = 1
        while true do
            local nl = rest:find("\n", start, true)
            if not nl then break end
            number = number + 1
            fn(rest:sub(start, nl - 1), number)
            start = nl + 1
        end
        rest = rest:sub(start)
        if #rest > 4096 then
            number = number + 1
            fn(rest, number)
            rest = ""
        end
    end
    if #rest > 0 then
        number = number + 1
        fn(rest, number)
    end
end

-- Leading option words (-i, -n, -c, or combined as -in); "--" ends
-- them. Returns the options and the index of the first other argument.
local function parse_options()
    local opts = { i = false, n = false, c = false }
    local k = 1
    while k <= #args do
        local word = args[k]
        if word == "--" then return opts, k + 1 end
        if not word:match("^%-%a+$") then break end
        for flag in word:sub(2):gmatch(".") do
            local f = flag:lower()
            if opts[f] == nil then return nil end
            opts[f] = true
        end
        k = k + 1
    end
    return opts, k
end

function setup()
    local opts, first = parse_options()
    if not opts or #args < first + 1 then
        UtilityResult(false, USAGE)
        return
    end
    local text = args[first]
    local needle = opts.i and text:lower() or text
    local files = {}
    for k = first + 1, #args do files[#files + 1] = card_path(args[k]) end
    local several = #files > 1

    local total, matched_files, searched = 0, 0, 0
    for _, path in ipairs(files) do
        local name = shown(path)
        local info = fs.stat(path)
        local f = info and not info.dir and fs.open(path, "r")
        if not f then
            emit("?" .. name .. ": " .. (not info and "NOT FOUND" or info.dir and "IS A DIRECTORY" or "CANNOT OPEN"))
        else
            searched = searched + 1
            local hits = 0
            local prefix = several and (name .. ":") or ""
            each_line(f, function(line, number)
                if line:sub(-1) == "\r" then line = line:sub(1, -2) end
                local hay = opts.i and line:lower() or line
                if hay:find(needle, 1, true) then
                    hits = hits + 1
                    if not opts.c then
                        emit(prefix .. (opts.n and (number .. ":") or "") .. line)
                    end
                end
            end)
            f:close()
            -- One file's count is the summary line itself.
            if opts.c and several then emit(prefix .. " " .. hits) end
            total = total + hits
            if hits > 0 then matched_files = matched_files + 1 end
        end
    end

    if searched == 0 then
        UtilityResult(false, listing[1] and listing[1]:sub(2) or USAGE)
        return
    end
    if dropped > 0 then listing[#listing + 1] = "... " .. dropped .. " more" end
    local summary = Text.Plural(total, "matching line")
    if several then summary = summary .. " in " .. Text.Plural(matched_files, "file") end
    UtilityResult(true, { message = summary:upper(), lines = listing })
end

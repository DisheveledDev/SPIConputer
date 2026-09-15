-- Walking: an explicit stack of directories, each listed once and
-- visited in name order, so the output reads like a sorted tree.

-- Listings stay small: the shell rebuilds the result table in its own
-- heap, which has little room to spare (8 KB is only the hard cap).
local MAX_LINES = 60
local BUDGET = 2500       -- bytes of listing
local MAX_VISITS = 3000
local MAX_DEPTH = 16

local function card_path(p)
    if p:sub(1, 1) == "/" then return p end
    return "/data/" .. p
end

local function shown(p)
    local s = p:gsub("^/data/?", "")
    if s == "" then return "/" end
    return s
end

-- A wildcard pattern as an anchored Lua pattern over lower-case names.
-- Without wildcards the text matches anywhere in the name.
local function compile(text)
    local pat = text:lower()
    if not pat:find("[%*%?]") then pat = "*" .. pat .. "*" end
    pat = pat:gsub("[%^%$%(%)%%%.%[%]%+%-]", "%%%0")
    pat = pat:gsub("%*", ".*"):gsub("%?", ".")
    return "^" .. pat .. "$"
end

local listing, listing_bytes, dropped = {}, 0, 0

local function emit(line)
    if dropped > 0 or #listing >= MAX_LINES or listing_bytes + #line + 8 > BUDGET then
        dropped = dropped + 1
        return
    end
    listing[#listing + 1] = line
    listing_bytes = listing_bytes + #line + 8
end

local function by_name(a, b)
    return a.name:lower() < b.name:lower()
end

function setup()
    if #args < 1 or #args > 2 then
        UtilityResult(false, "USAGE: FIND PATTERN [DIR]")
        return
    end
    local pattern = compile(args[1]:match("[^/]*$"))
    local root = args[2] and card_path(args[2]) or "/data"
    local info = fs.stat(root)
    if not info or not info.dir then
        UtilityResult(false, shown(root) .. ": NOT A DIRECTORY")
        return
    end

    -- Each stack entry is a directory still to list, with its depth.
    local stack = { { path = root, depth = 0 } }
    local visits, found, stopped = 0, 0, false
    while #stack > 0 do
        local dir = table.remove(stack)
        local entries = fs.ls(dir.path) or {}
        table.sort(entries, by_name)
        local subdirs = {}
        for _, entry in ipairs(entries) do
            visits = visits + 1
            if visits > MAX_VISITS then
                stopped = true
                break
            end
            local path = (dir.path == "/" and "" or dir.path) .. "/" .. entry.name
            if entry.name:lower():find(pattern) then
                found = found + 1
                emit(shown(path) .. (entry.dir and "/" or ""))
            end
            if entry.dir and dir.depth < MAX_DEPTH then
                subdirs[#subdirs + 1] = { path = path, depth = dir.depth + 1 }
            end
        end
        if stopped then break end
        -- Pushed in reverse so the first subdirectory is walked next.
        for k = #subdirs, 1, -1 do stack[#stack + 1] = subdirs[k] end
    end

    if dropped > 0 then listing[#listing + 1] = "... " .. dropped .. " more" end
    if stopped then listing[#listing + 1] = "(stopped after " .. MAX_VISITS .. " entries)" end
    local message = found == 0 and "NO MATCHES" or Text.Plural(found, "match", "matches"):upper()
    UtilityResult(true, { message = message, lines = listing })
end

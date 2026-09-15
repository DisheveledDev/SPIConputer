-- The walk is recursive (12 levels at most); each directory is listed
-- once, then its entries are drawn with the prefix their depth needs.

-- Listings stay small: the shell rebuilds the result table in its own
-- heap, which has little room to spare (8 KB is only the hard cap).
local MAX_LINES = 60
local BUDGET = 2500       -- bytes of listing
local MAX_DEPTH = 12
local MAX_VISITS = 3000
local WIDTH = 39          -- keep lines inside the 40-column screen

local function card_path(p)
    if p:sub(1, 1) == "/" then return p end
    return "/data/" .. p
end

local function shown(p)
    local s = p:gsub("^/data/?", "")
    if s == "" then return "/" end
    return s
end

local function size_text(n)
    if n < 1024 then return tostring(n) end
    if n < 10 * 1024 then return string.format("%.1fK", n / 1024) end
    if n < 1024 * 1024 then return (n // 1024) .. "K" end
    return string.format("%.1fM", n / (1024 * 1024))
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

-- Directories first, then files, each by name ignoring case.
local function order(a, b)
    if a.dir ~= b.dir then return a.dir end
    return a.name:lower() < b.name:lower()
end

local dirs, files, bytes, visits = 0, 0, 0, 0

local function walk(path, prefix, depth)
    local entries = fs.ls(path) or {}
    table.sort(entries, order)
    for k, entry in ipairs(entries) do
        visits = visits + 1
        if visits > MAX_VISITS then return end
        local last = k == #entries
        local suffix = entry.dir and "/" or ("  " .. size_text(entry.size))
        local room = WIDTH - #prefix - 4 - #suffix
        local name = entry.name
        if #name > room and room > 2 then name = name:sub(1, room - 2) .. ".." end
        emit(prefix .. "+-- " .. name .. suffix)
        if entry.dir then
            dirs = dirs + 1
            if depth < MAX_DEPTH then
                walk(path .. "/" .. entry.name, prefix .. (last and "    " or "|   "), depth + 1)
            end
        else
            files = files + 1
            bytes = bytes + entry.size
        end
    end
end

function setup()
    if #args > 1 then
        UtilityResult(false, "USAGE: TREE [DIR]")
        return
    end
    local root = args[1] and card_path(args[1]) or "/data"
    local info = fs.stat(root)
    if not info or not info.dir then
        UtilityResult(false, shown(root) .. ": NOT A DIRECTORY")
        return
    end
    walk(root == "/" and "" or root, "", 1)
    if dropped > 0 then listing[#listing + 1] = "... " .. dropped .. " more" end
    if visits > MAX_VISITS then listing[#listing + 1] = "(stopped after " .. MAX_VISITS .. " entries)" end
    listing[#listing + 1] = string.format("%s, %s, %s BYTES",
        Text.Plural(dirs, "DIR", "DIRS"), Text.Plural(files, "FILE", "FILES"), Text.Commas(bytes))
    local title = shown(root)
    if title ~= "/" then title = title .. "/" end
    UtilityResult(true, { message = title, lines = listing })
end

-- Reading backwards: chunks are prepended until the text read holds
-- `count` complete lines (count newlines before the final line), or 16
-- KB, whichever comes first.

local CHUNK = 1024
local MAX_TEXT = 16 * 1024
local DEFAULT_LINES = 10
-- Listings stay small: the shell rebuilds the result table in its own
-- heap, which has little room to spare (8 KB is only the hard cap).
local MAX_LINES = 60
local BUDGET = 2500       -- bytes of listing
local LONGEST = 200
local USAGE = "USAGE: TAIL [-N] FILE"

local function card_path(p)
    if p:sub(1, 1) == "/" then return p end
    return "/data/" .. p
end

local function shown(p)
    local s = p:gsub("^/data/?", "")
    if s == "" then return "/" end
    return s
end

-- "-20", or "-n 20": the line count and the file argument.
local function parse()
    local count, k = DEFAULT_LINES, 1
    if args[k] and args[k]:lower() == "-n" then
        count = tonumber(args[k + 1] or "")
        k = k + 2
    elseif args[k] and args[k]:match("^%-%d+$") then
        count = tonumber(args[k]:sub(2))
        k = k + 1
    end
    if not count or count < 1 or k ~= #args then return nil end
    return math.min(math.floor(count), MAX_LINES), args[k]
end

function setup()
    local count, file = parse()
    if not count then
        UtilityResult(false, USAGE)
        return
    end
    local path = card_path(file)
    local info = fs.stat(path)
    local f = info and not info.dir and fs.open(path, "r")
    if not f then
        UtilityResult(false, shown(path) .. (not info and ": NOT FOUND" or info.dir and ": IS A DIRECTORY" or ": CANNOT OPEN"))
        return
    end

    local size = f:size()
    local pos = size
    local pieces = {}          -- chunks read, last chunk first
    local held, newlines = 0, 0
    local trailing = nil       -- does the file end with a newline?
    while pos > 0 and held < MAX_TEXT do
        local len = math.min(CHUNK, pos)
        pos = pos - len
        f:seek(pos, "set")
        local chunk = f:read(len) or ""
        if trailing == nil then trailing = chunk:sub(-1) == "\n" end
        pieces[#pieces + 1] = chunk
        held = held + #chunk
        for _ in chunk:gmatch("\n") do newlines = newlines + 1 end
        if newlines - (trailing and 1 or 0) >= count then break end
    end
    f:close()

    -- Reassemble in file order and split; the first piece may start
    -- mid-line when the file continues before it.
    local ordered = {}
    for k = #pieces, 1, -1 do ordered[#ordered + 1] = pieces[k] end
    local text = table.concat(ordered)
    if trailing then text = text:sub(1, -2) end
    local all = {}
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do all[#all + 1] = line end
    local first = math.max(1, #all - count + 1)
    if pos > 0 and first == 1 and #all > 1 then first = 2 end

    local lines, bytes = {}, 0
    for k = first, #all do
        local line = all[k]:gsub("\r$", ""):gsub("%c", ".")
        if #line > LONGEST then line = line:sub(1, LONGEST - 2) .. ".." end
        lines[#lines + 1] = line
        bytes = bytes + #line + 8
    end
    -- Over budget: keep the end, which is what tail is for.
    while bytes > BUDGET and #lines > 1 do
        bytes = bytes - #table.remove(lines, 1) - 8
    end
    if size == 0 or #lines == 0 then
        UtilityResult(true, "(EMPTY FILE)")
        return
    end
    UtilityResult(true, { lines = lines })
end

-- Reads chunks until enough lines have been seen, then stops.

local CHUNK = 1024
local DEFAULT_LINES = 10
-- Listings stay small: the shell rebuilds the result table in its own
-- heap, which has little room to spare (8 KB is only the hard cap).
local MAX_LINES = 60
local BUDGET = 2500       -- bytes of listing
local LONGEST = 200
local USAGE = "USAGE: HEAD [-N] FILE"

local function card_path(p)
    if p:sub(1, 1) == "/" then return p end
    return "/data/" .. p
end

local function shown(p)
    local s = p:gsub("^/data/?", "")
    if s == "" then return "/" end
    return s
end

-- "-20", or "-n 20": the line count and the index of the file argument.
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

    local lines, bytes, rest = {}, 0, ""
    local function add(line)
        line = line:gsub("\r$", ""):gsub("%c", ".")
        if #line > LONGEST then line = line:sub(1, LONGEST - 2) .. ".." end
        if bytes + #line + 8 > BUDGET then return false end
        lines[#lines + 1] = line
        bytes = bytes + #line + 8
        return #lines < count
    end
    local more = true
    while more do
        local chunk = f:read(CHUNK)
        if not chunk or chunk == "" then break end
        rest = rest .. chunk
        local start = 1
        while more do
            local nl = rest:find("\n", start, true)
            if not nl then break end
            more = add(rest:sub(start, nl - 1))
            start = nl + 1
        end
        rest = rest:sub(start)
        if #rest > 4096 then
            more = add(rest)
            rest = ""
        end
    end
    if more and #rest > 0 then add(rest) end
    f:close()
    if #lines == 0 then
        UtilityResult(true, "(EMPTY FILE)")
        return
    end
    UtilityResult(true, { lines = lines })
end

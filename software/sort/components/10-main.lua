-- The file is read as one string and the lines are sorted as one
-- integer each (start * 32768 + length), not as a string each: a Lua
-- string costs ~40 bytes of header per line, which would cap sorting at a
-- few hundred lines. Lines are cut out of the text only to compare,
-- print or write them.

local MAX_SIZE = 32 * 1024 - 1     -- keeps start * 32768 inside 32-bit integers
local MAX_SORT_LINES = 2000
local SPLIT = 32768
local CHUNK = 1024
-- Listings stay small: the shell rebuilds the result table in its own
-- heap, which has little room to spare (8 KB is only the hard cap).
local MAX_LINES = 60
local BUDGET = 2500       -- bytes of listing
local LONGEST = 200
local USAGE = "USAGE: SORT [-R] [-N] FILE [OUTFILE]"

local function card_path(p)
    if p:sub(1, 1) == "/" then return p end
    return "/data/" .. p
end

local function shown(p)
    local s = p:gsub("^/data/?", "")
    if s == "" then return "/" end
    return s
end

local text = ""           -- the whole file

-- The line an index entry names.
local function line_of(k)
    local start = k // SPLIT
    return text:sub(start, start + k % SPLIT - 1)
end

-- Lines in the text, counted without copying it.
local function count_lines()
    local n, pos, size = 0, 1, #text
    while pos <= size do
        n = n + 1
        pos = (text:find("\n", pos, true) or size) + 1
    end
    return n
end

-- One entry per line, a trailing carriage return left out. The table is
-- sized once (Lua 5.5's table.create): growing it by doubling would hold
-- the old and new arrays at the same time.
local function index_lines(count)
    local index = table.create and table.create(count) or {}
    local pos, size, n = 1, #text, 0
    while pos <= size do
        local nl = text:find("\n", pos, true) or size + 1
        local stop = nl - 1
        if stop >= pos and text:byte(stop) == 13 then stop = stop - 1 end
        n = n + 1
        index[n] = pos * SPLIT + (stop - pos + 1)
        pos = nl + 1
    end
    return index
end

local function write_lines(path, index)
    local f, err = fs.open(path, "w")
    if not f then return nil, tostring(err) end
    local batch, size = {}, 0
    for _, k in ipairs(index) do
        local line = line_of(k)
        batch[#batch + 1] = line
        size = size + #line + 1
        if size >= CHUNK then
            f:write(table.concat(batch, "\n") .. "\n")
            batch, size = {}, 0
        end
    end
    if #batch > 0 then f:write(table.concat(batch, "\n") .. "\n") end
    f:close()
    return true
end

-- The number a line starts with (after spaces), or nil.
local function leading_number(line)
    return tonumber(line:match("^%s*([-+]?%d+%.?%d*)"))
end

-- Lines that start with a number sort before those that do not, in
-- either direction; ties fall back to text order.
local function comparator(numeric, reverse)
    if not numeric then
        if reverse then return function(a, b) return line_of(a) > line_of(b) end end
        return function(a, b) return line_of(a) < line_of(b) end
    end
    return function(i, j)
        local a, b = line_of(i), line_of(j)
        local x, y = leading_number(a), leading_number(b)
        if x and y then
            if x ~= y then
                if reverse then return x > y end
                return x < y
            end
        elseif x or y then
            return x ~= nil
        end
        if reverse then return a > b end
        return a < b
    end
end

-- The whole job: returns ok and the result for UtilityResult.
local function run()
    local opts = { r = false, n = false }
    local k = 1
    while args[k] and args[k]:match("^%-%a+$") do
        for flag in args[k]:sub(2):gmatch(".") do
            local f = flag:lower()
            if opts[f] == nil then return false, USAGE end
            opts[f] = true
        end
        k = k + 1
    end
    local source, target = args[k], args[k + 1]
    if not source or args[k + 2] then return false, USAGE end
    source = card_path(source)
    local info = fs.stat(source)
    if not info or info.dir then
        return false, shown(source) .. (info and ": IS A DIRECTORY" or ": NOT FOUND")
    end
    if info.size > MAX_SIZE then
        return false, string.format("%s: TOO BIG TO SORT (%s KB MAX)", shown(source), MAX_SIZE // 1024)
    end

    local data, err = fs.readall(source)
    if not data then return false, tostring(err) end
    text = data
    local count = count_lines()
    if count > MAX_SORT_LINES then
        return false, string.format("%s: TOO MANY LINES (%d MAX)", shown(source), MAX_SORT_LINES)
    end
    local index = index_lines(count)
    if #index == 0 then return true, "(EMPTY FILE)" end
    table.sort(index, comparator(opts.n, opts.r))

    if target then
        target = card_path(target)
        local ok, write_err = write_lines(target, index)
        if not ok then return false, shown(target) .. ": " .. write_err end
        return true, string.format("SORTED %s TO %s", Text.Plural(#index, "LINE", "LINES"), shown(target))
    end

    local shown_lines, bytes = {}, 0
    for i, k in ipairs(index) do
        local line = line_of(k):gsub("%c", ".")
        if #line > LONGEST then line = line:sub(1, LONGEST - 2) .. ".." end
        if i > MAX_LINES or bytes + #line + 8 > BUDGET then
            shown_lines[#shown_lines + 1] = "... " .. (#index - i + 1) .. " more (sort to a file)"
            break
        end
        shown_lines[#shown_lines + 1] = line
        bytes = bytes + #line + 8
    end
    return true, { lines = shown_lines }
end

-- Many short lines can outgrow the heap even under the size limit:
-- running out of memory is caught and reported, and the file being
-- written is left as far as it got.
function setup()
    local ran, ok, result = pcall(run)
    if not ran then
        local err = tostring(ok)
        collectgarbage()
        if err:find("not enough memory", 1, true) then
            err = "NOT ENOUGH MEMORY: TOO MANY LINES"
        end
        UtilityResult(false, err)
        return
    end
    UtilityResult(ok, result)
end

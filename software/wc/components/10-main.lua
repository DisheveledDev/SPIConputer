-- Counting is streamed a kilobyte at a time. A word split across two
-- chunks is counted once: it is only new if the previous chunk did not
-- end inside a word.

local CHUNK = 1024

local function card_path(p)
    if p:sub(1, 1) == "/" then return p end
    return "/data/" .. p
end

-- A card path as the user thinks of it: relative to /data.
local function shown(p)
    local s = p:gsub("^/data/?", "")
    if s == "" then return "/" end
    return s
end

local function count(path)
    local info = fs.stat(path)
    if not info then return nil, "NOT FOUND" end
    if info.dir then return nil, "IS A DIRECTORY" end
    local f, err = fs.open(path, "r")
    if not f then return nil, tostring(err) end
    local lines, words, bytes = 0, 0, 0
    local in_word = false
    while true do
        local chunk = f:read(CHUNK)
        if not chunk or chunk == "" then break end
        bytes = bytes + #chunk
        for _ in chunk:gmatch("\n") do lines = lines + 1 end
        local n = 0
        for _ in chunk:gmatch("%S+") do n = n + 1 end
        if in_word and chunk:find("^%S") then n = n - 1 end
        words = words + n
        in_word = chunk:find("%S$") ~= nil
    end
    f:close()
    return lines, words, bytes
end

local function row(l, w, b, name)
    return string.format("%6d %6d %8d %s", l, w, b, name)
end

function setup()
    if #args == 0 then
        UtilityResult(false, "USAGE: WC FILE...")
        return
    end
    local out = {}
    local tl, tw, tb, counted = 0, 0, 0, 0
    local first_error
    for i = 1, #args do
        local path = card_path(args[i])
        local l, w, b = count(path)
        if l then
            out[#out + 1] = row(l, w, b, shown(path))
            tl, tw, tb, counted = tl + l, tw + w, tb + b, counted + 1
        else
            local message = shown(path) .. ": " .. w
            out[#out + 1] = "?" .. message
            first_error = first_error or message
        end
    end
    if counted == 0 then
        UtilityResult(false, first_error)
        return
    end
    if counted > 1 then
        out[#out + 1] = row(tl, tw, tb, "TOTAL")
    end
    UtilityResult(true, {
        message = string.format("%6s %6s %8s %s", "LINES", "WORDS", "BYTES", "FILE"),
        lines = out,
    })
end

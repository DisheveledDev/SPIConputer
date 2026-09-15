-- One seek and one read: only the bytes asked for are loaded.

local PER_LINE = 8
local DEFAULT_LENGTH = 256
local MAX_LENGTH = 320       -- 40 lines: what the shell can take in one result
local USAGE = "USAGE: HEXDUMP FILE [OFFSET] [LENGTH]"

local function card_path(p)
    if p:sub(1, 1) == "/" then return p end
    return "/data/" .. p
end

local function shown(p)
    local s = p:gsub("^/data/?", "")
    if s == "" then return "/" end
    return s
end

-- A whole number >= 0 from decimal or 0x hex text, or nil.
local function number_arg(text)
    local n = tonumber(text)
    if not n or n < 0 or n ~= math.floor(n) then return nil end
    return math.tointeger(n)
end

local function dump_line(offset, bytes, wide)
    local hex = bytes:gsub(".", function(c) return string.format("%02X ", c:byte()) end)
    hex = hex .. string.rep("   ", PER_LINE - #bytes)
    local text = bytes:gsub("[^\32-\126]", ".")
    return string.format(wide and "%06X: %s%s" or "%04X: %s%s", offset, hex, text)
end

function setup()
    if #args < 1 or #args > 3 then
        UtilityResult(false, USAGE)
        return
    end
    local path = card_path(args[1])
    local offset, length = 0, DEFAULT_LENGTH
    if args[2] then offset = number_arg(args[2]) end
    if args[3] then length = number_arg(args[3]) end
    if not offset or not length or length == 0 then
        UtilityResult(false, USAGE)
        return
    end
    local info = fs.stat(path)
    if not info or info.dir then
        UtilityResult(false, shown(path) .. (info and ": IS A DIRECTORY" or ": NOT FOUND"))
        return
    end
    if offset >= info.size then
        UtilityResult(false, string.format("OFFSET PAST END (%s BYTES)", Text.Commas(info.size)))
        return
    end
    local clipped = length > MAX_LENGTH
    length = math.min(length, MAX_LENGTH, info.size - offset)

    local f, err = fs.open(path, "r")
    if not f then
        UtilityResult(false, tostring(err))
        return
    end
    f:seek(offset, "set")
    local data = f:read(length) or ""
    f:close()

    local wide = offset + #data > 0x10000
    local lines = {}
    for at = 1, #data, PER_LINE do
        lines[#lines + 1] = dump_line(offset + at - 1, data:sub(at, at + PER_LINE - 1), wide)
    end
    if clipped then lines[#lines + 1] = "(320 BYTES AT MOST: STEP THE OFFSET)" end
    UtilityResult(true, {
        message = string.format("%s: %s BYTES", shown(path):upper(), Text.Commas(info.size)),
        lines = lines,
    })
end

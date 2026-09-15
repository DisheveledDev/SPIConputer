-- Card paths: the shell passes data entries and file-name-like words
-- as full paths (/data/...); anything else is relative to /data.
local function card_path(path)
    return path:sub(1, 1) == "/" and path or "/data/" .. path
end

local function shown(path)
    local s = path:gsub("^/data", "")
    return s == "" and "/" or s
end

local function base_name(path)
    return path:match("([^/]+)$") or path
end

function setup()
    if not args[1] then
        UtilityResult(false, "USAGE: COMPILE <FILE.LUA> [OUT.PRG]")
        return
    end
    local source = card_path(args[1])
    local target = args[2] and card_path(args[2]) or nil
    local ok, result = Compile(source, target)
    if not ok then
        UtilityResult(false, tostring(result))
        return
    end
    local out = target or (source:gsub("%.[Ll][Uu][Aa]$", "") .. ".prg")
    UtilityResult(true, shown(out) .. ": " .. Text.Commas(result) .. " BYTES")
end

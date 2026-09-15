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

local function describe(path, lines)
    local info, err = fs.stat(path)
    if not info then
        lines[#lines + 1] = "?" .. shown(path) .. ": " .. tostring(err)
        return
    end
    lines[#lines + 1] = "PATH: " .. shown(path)
    if info.dir then
        local files, dirs, bytes = 0, 0, 0
        for _, entry in ipairs(fs.ls(path) or {}) do
            if entry.dir then dirs = dirs + 1 else files, bytes = files + 1, bytes + entry.size end
        end
        lines[#lines + 1] = "TYPE: DIRECTORY"
        lines[#lines + 1] = string.format("HOLDS: %s, %s", Text.Plural(files, "FILE", "FILES"),
                                          Text.Plural(dirs, "DIR", "DIRS"))
        lines[#lines + 1] = "SIZE: " .. Text.Commas(bytes) .. " BYTES (FILES HERE)"
    else
        lines[#lines + 1] = "TYPE: FILE"
        lines[#lines + 1] = "SIZE: " .. Text.Commas(info.size) .. " BYTES"
    end
end

function setup()
    if #args == 0 then
        UtilityResult(false, "USAGE: STAT <PATH>...")
        return
    end
    local lines = {}
    for i, arg in ipairs(args) do
        if i > 1 then lines[#lines + 1] = "" end
        describe(card_path(arg), lines)
    end
    UtilityResult(true, { lines = lines })
end

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
    if #args == 0 then
        UtilityResult(false, "USAGE: DEL <FILE>...")
        return
    end
    local removed, lines = 0, {}
    for _, arg in ipairs(args) do
        local path = card_path(arg)
        local info = fs.stat(path)
        if not info then
            lines[#lines + 1] = "?NOT FOUND: " .. shown(path)
        elseif info.dir then
            lines[#lines + 1] = "?" .. shown(path) .. " IS A DIRECTORY (USE RD)"
        else
            local ok, err = fs.remove(path)
            if ok then
                removed = removed + 1
            else
                lines[#lines + 1] = "?" .. shown(path) .. ": " .. tostring(err)
            end
        end
    end
    UtilityResult(#lines == 0, {
        message = Text.Plural(removed, "FILE", "FILES") .. " DELETED",
        lines = lines,
    })
end

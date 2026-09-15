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

-- Make `path` and any missing parents. Returns true or nil, err.
local function make_all(path)
    local built = ""
    for part in path:gmatch("[^/]+") do
        built = built .. "/" .. part
        local info = fs.stat(built)
        if not info then
            local ok, err = fs.mkdir(built)
            if not ok then return nil, err end
        elseif not info.dir then
            return nil, shown(built) .. " IS A FILE"
        end
    end
    return true
end

function setup()
    if #args == 0 then
        UtilityResult(false, "USAGE: MD <DIRECTORY>...")
        return
    end
    local lines = {}
    for _, arg in ipairs(args) do
        local ok, err = make_all(card_path(arg))
        if not ok then lines[#lines + 1] = "?" .. arg .. ": " .. tostring(err) end
    end
    UtilityResult(#lines == 0, { lines = lines })
end

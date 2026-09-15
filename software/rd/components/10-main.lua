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

-- Remove a directory tree; returns files, dirs removed or nil, err.
local function remove_tree(path)
    local files, dirs = 0, 0
    local entries, err = fs.ls(path)
    if not entries then return nil, err end
    for _, entry in ipairs(entries) do
        local child = path .. "/" .. entry.name
        if entry.dir then
            local f, d = remove_tree(child)
            if not f then return nil, d end
            files, dirs = files + f, dirs + d
        else
            local ok, remove_err = fs.remove(child)
            if not ok then return nil, remove_err end
            files = files + 1
        end
    end
    local ok, remove_err = fs.remove(path)
    if not ok then return nil, remove_err end
    return files, dirs + 1
end

function setup()
    local recursive = false
    local targets = {}
    for _, arg in ipairs(args) do
        if arg:lower() == "-s" then recursive = true else targets[#targets + 1] = arg end
    end
    if #targets == 0 then
        UtilityResult(false, "USAGE: RD [-S] <DIRECTORY>...")
        return
    end
    local files, dirs, lines = 0, 0, {}
    for _, arg in ipairs(targets) do
        local path = card_path(arg)
        local info = fs.stat(path)
        if path == "/data" or path == "/data/" then
            lines[#lines + 1] = "?THE DATA FOLDER STAYS"
        elseif not info then
            lines[#lines + 1] = "?NOT FOUND: " .. shown(path)
        elseif not info.dir then
            lines[#lines + 1] = "?" .. shown(path) .. " IS A FILE (USE DEL)"
        elseif recursive then
            local f, d = remove_tree(path)
            if f then
                files, dirs = files + f, dirs + d
            else
                lines[#lines + 1] = "?" .. shown(path) .. ": " .. tostring(d)
            end
        else
            local ok, err = fs.remove(path)
            if ok then
                dirs = dirs + 1
            else
                lines[#lines + 1] = "?" .. shown(path) .. ": NOT EMPTY (RD -S)"
            end
        end
    end
    UtilityResult(#lines == 0, {
        message = recursive and (Text.Plural(dirs, "DIR", "DIRS") .. ", "
            .. Text.Plural(files, "FILE", "FILES") .. " REMOVED") or nil,
        lines = lines,
    })
end

local MAX_LINES = 150      -- keeps the result well inside its 8 KB

local function shown(path)
    local s = path:gsub("^/data", "")
    return s == "" and "/" or s
end

-- DOS-style wildcards (* and ?) to an anchored, case-insensitive pattern.
local function wildcard(pattern)
    local lua = pattern:lower():gsub("[%^%$%(%)%%%.%[%]%+%-]", "%%%0")
    lua = lua:gsub("%*", ".*"):gsub("%?", ".")
    return "^" .. lua .. "$"
end

function setup()
    local target = args[1] or "/data"
    if target:sub(1, 1) ~= "/" then target = "/data/" .. target end
    local dir, pattern = target, nil
    if target:find("[*?]") then
        dir, pattern = target:match("^(.*)/([^/]*)$")
    end
    local entries, err = fs.ls(dir)
    if not entries then
        UtilityResult(false, shown(dir) .. ": " .. tostring(err))
        return
    end
    local match = pattern and wildcard(pattern)
    local list = {}
    for _, entry in ipairs(entries) do
        if not match or entry.name:lower():match(match) then list[#list + 1] = entry end
    end
    table.sort(list, function(a, b)
        if a.dir ~= b.dir then return a.dir end
        return a.name:lower() < b.name:lower()
    end)
    local lines = {}
    local files, dirs, bytes = 0, 0, 0
    for _, entry in ipairs(list) do
        local line
        if entry.dir then
            dirs = dirs + 1
            line = string.format("  %-26s <DIR>", entry.name)
        else
            files, bytes = files + 1, bytes + entry.size
            line = string.format("  %-26s%10s", entry.name, Text.Commas(entry.size))
        end
        if #lines < MAX_LINES then lines[#lines + 1] = line end
    end
    if #list > MAX_LINES then
        lines[#lines + 1] = "  ... " .. (#list - MAX_LINES) .. " MORE"
    end
    lines[#lines + 1] = string.format("%s, %s, %s BYTES", Text.Plural(files, "FILE", "FILES"),
                                      Text.Plural(dirs, "DIR", "DIRS"), Text.Commas(bytes))
    UtilityResult(true, {
        message = "DIRECTORY OF " .. shown(dir)
            .. (pattern and ((shown(dir) == "/" and "" or "/") .. pattern) or ""),
        lines = lines,
    })
end

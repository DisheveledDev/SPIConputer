-- The listing: read a folder, folders first, names without case.

local function join(dir, name)
    return dir == "/" and "/" .. name or dir .. "/" .. name
end

local function shown(path)
    local s = path:gsub("^" .. ROOT, "")
    return s == "" and "/" or s
end

-- A typed name as a card path: "/x" is relative to data, "x" to the
-- current folder.
local function resolve(name)
    if name:sub(1, 1) == "/" then return ROOT .. name end
    return join(cwd, name)
end

local function by_kind_then_name(a, b)
    if a.dir ~= b.dir then return a.dir end
    return a.name:lower() < b.name:lower()
end

-- Re-read cwd; select `keep` by name if it is still there. Returns an
-- error message when the folder cannot be read (or is too big for the
-- heap: fs.ls builds every entry at once, so it runs under pcall).
local function load_entries(keep)
    names, sizes, count = {}, {}, 0
    collectgarbage("collect")
    local ok, list, err = pcall(fs.ls, cwd)
    if not ok then list, err = nil, "folder too large to list" end
    if cwd ~= ROOT then
        count = 1
        names[1], sizes[1] = "..", UP
    end
    if list then
        table.sort(list, by_kind_then_name)
        for i = 1, #list do
            local e = list[i]
            count = count + 1
            names[count] = e.name
            sizes[count] = e.dir and FOLDER or e.size
            list[i] = nil
        end
        list = nil
    end
    sel = 1
    for i = 1, count do
        if names[i] == keep then sel = i break end
    end
    top = math.max(1, math.min(sel - LIST_H // 2, count - LIST_H + 1))
    return list == nil and err and tostring(err) or nil
end

local function is_dir(i)
    return sizes[i] < 0
end

local function entry_attr(i)
    local size = sizes[i]
    if size == UP then return UP_ATTR end
    return size == FOLDER and DIR_ATTR or FILE_ATTR
end

local function size_text(i)
    local size = sizes[i]
    if size == UP then return "<UP>" end
    if size == FOLDER then return "<DIR>" end
    if size < 10000000 then return Text.Commas(size) end
    return Text.Commas(size // 1048576) .. " MB"
end

local function row_text(i)
    return string.format(" %-27.27s %10s ", names[i], size_text(i))
end

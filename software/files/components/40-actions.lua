-- Actions on the selected entry.

-- Re-read the folder and redraw, keeping the selection by name.
local function refresh(keep)
    local err = load_entries(keep)
    draw_all()
    if err then message("CANNOT READ", err) end
end

local function change_dir(path, keep)
    local old = cwd
    cwd = path
    local err = load_entries(keep)
    if err then
        cwd = old
        load_entries()
        message("CANNOT OPEN", err)
    end
    draw_all()
end

local function go_up()
    if cwd == ROOT then return end
    local name = cwd:match("([^/]+)$")
    change_dir(cwd:match("^(.*)/[^/]+$"), name)
end

-- Run another installed app on this file; the listing is re-read when
-- it returns (tick). Nothing is drawn after Execute: the other app owns
-- the display until it exits.
local function run_app(app, path)
    local folder = "/apps/" .. app .. ".app/"
    local program = fs.exists(folder .. "app.prg") and folder .. "app.prg"
                    or fs.exists(folder .. "app.lua") and folder .. "app.lua"
    if not program then
        message("NOT INSTALLED", app .. " is not in /apps")
        return
    end
    child_running = true
    local ok, err = Execute(program, path)
    if not ok then
        child_running = false
        message("CANNOT RUN", tostring(err))
    end
end

-- The selected entry's name, unless it is ".." (or nothing).
local function selected_name()
    if sel > count or sizes[sel] == UP then return nil end
    return names[sel]
end

local function open_selected()
    if sel > count then return end
    if sizes[sel] == UP then
        go_up()
    elseif is_dir(sel) then
        change_dir(join(cwd, names[sel]))
    else
        run_app("view", join(cwd, names[sel]))
    end
end

local function edit_selected()
    local name = selected_name()
    if name and not is_dir(sel) then run_app("editor", join(cwd, name)) end
end

local function delete_selected()
    local name = selected_name()
    if not name then return end
    local folder = is_dir(sel)
    confirm("Delete " .. name:sub(1, 24) .. "?", function()
        local ok, err = fs.remove(join(cwd, name))
        if not ok then
            message("CANNOT DELETE", folder and "folder not empty?" or tostring(err))
            return
        end
        refresh(names[sel + 1] or names[sel - 1])
    end)
end

local function rename_selected()
    local old = selected_name()
    if not old then return end
    ask("RENAME", "New name or /path:", old, function(name)
        if name == old then return end
        local target = resolve(name)
        if fs.exists(target) then
            message("CANNOT RENAME", name .. " already exists")
            return
        end
        local ok, err = fs.rename(join(cwd, old), target)
        if not ok then
            message("CANNOT RENAME", tostring(err))
            return
        end
        refresh(name)
    end)
end

-- Stream the copy 1 KB at a time: files bigger than the heap copy too.
local function copy_file(from, to)
    local src, err = fs.open(from, "r")
    if not src then return nil, err end
    local dst
    dst, err = fs.open(to, "w")
    if not dst then
        src:close()
        return nil, err
    end
    while true do
        local chunk = src:read(1024)
        if not chunk or #chunk == 0 then break end
        dst:write(chunk)
    end
    src:close()
    dst:close()
    return true
end

local function copy_selected()
    local from = selected_name()
    if not from or is_dir(sel) then return end
    ask("COPY", "Copy to name or /path:", "copy-" .. from, function(name)
        local target = resolve(name)
        if fs.exists(target) then
            message("CANNOT COPY", name .. " already exists")
            return
        end
        local ok, err = copy_file(join(cwd, from), target)
        if not ok then
            message("CANNOT COPY", tostring(err))
            return
        end
        refresh(name)
    end)
end

local function make_dir()
    ask("NEW FOLDER", "Folder name:", "", function(name)
        local ok, err = fs.mkdir(resolve(name))
        if not ok then
            message("CANNOT CREATE", tostring(err))
            return
        end
        refresh(name)
    end)
end

-- Gathers each figure with one or two fs calls; the result is a table,
-- which the shell prints as sorted KEY = VALUE lines.

local MAX_DEPTH = 16
local MAX_VISITS = 3000

local function uptime()
    local s = TimeNow() // 1000
    return string.format("%d:%02d:%02d", s // 3600, s // 60 % 60, s % 60)
end

-- Installed bundles (<name><ext> folders) and loose programs in a root.
local function count_programs(root, ext)
    local n = 0
    for _, entry in ipairs(fs.ls(root) or {}) do
        local lower = entry.name:lower()
        if entry.dir and lower:sub(-#ext) == ext then
            n = n + 1
        elseif not entry.dir and (lower:match("%.prg$") or lower:match("%.lua$")) then
            n = n + 1
        end
    end
    return n
end

-- Files (not directories) under a folder, with an explicit stack.
local function count_files(root)
    local files, visits = 0, 0
    local stack = { { root, 0 } }
    while #stack > 0 do
        local dir = table.remove(stack)
        for _, entry in ipairs(fs.ls(dir[1]) or {}) do
            visits = visits + 1
            if visits > MAX_VISITS then return files, true end
            if entry.dir then
                if dir[2] < MAX_DEPTH then
                    stack[#stack + 1] = { dir[1] .. "/" .. entry.name, dir[2] + 1 }
                end
            else
                files = files + 1
            end
        end
    end
    return files, false
end

local function megabytes(kb)
    return Text.Commas(kb // 1024) .. " MB"
end

function setup()
    local int_bits = math.maxinteger > 2147483647 and 64 or 32
    local float_bits = (2 ^ 24 + 1 == 2 ^ 24) and 32 or 64
    local info = {
        message = "SPICOMPUTER OS",
        lua = _VERSION,
        integers = int_bits .. "-BIT",
        floats = float_bits .. "-BIT",
        uptime = uptime(),
        screen = "40 X 30 TEXT",
        apps = count_programs("/apps", ".app"),
        utils = count_programs("/utils", ".util"),
        games = count_programs("/games", ".game"),
    }
    local free, total = fs.free()
    if free and total then
        info.card = megabytes(free) .. " FREE OF " .. megabytes(total)
    else
        info.card = "NOT MOUNTED"
    end
    local files, capped = count_files("/data")
    info.data_files = capped and (files .. "+") or files
    UtilityResult(true, info)
end

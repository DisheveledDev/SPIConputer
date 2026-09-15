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
    if #args < 2 then
        UtilityResult(false, "USAGE: REN <OLD>... <NEW|DIR>")
        return
    end
    local target = card_path(args[#args])
    local info = fs.stat(target)
    local into_dir = info and info.dir
    if #args > 2 and not into_dir then
        UtilityResult(false, "MOVING SEVERAL ENTRIES NEEDS A DIRECTORY")
        return
    end
    local moved, lines = 0, {}
    for i = 1, #args - 1 do
        local source = card_path(args[i])
        local dest = into_dir and (target .. "/" .. base_name(source)) or target
        local ok, err = fs.rename(source, dest)
        if ok then
            moved = moved + 1
        else
            lines[#lines + 1] = "?" .. shown(source) .. ": " .. tostring(err)
        end
    end
    if #args == 2 and moved == 1 then
        UtilityResult(true, shown(card_path(args[1])) .. " -> " .. shown(target))
        return
    end
    UtilityResult(#lines == 0, { message = moved .. " MOVED", lines = lines })
end

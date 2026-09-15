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

local function copy_file(source, target)
    local fin, err = fs.open(source, "r")
    if not fin then return nil, err end
    local fout, out_err = fs.open(target, "w")
    if not fout then
        fin:close()
        return nil, out_err
    end
    local total = 0
    while true do
        local chunk = fin:read(1024)
        if not chunk or chunk == "" then break end
        fout:write(chunk)
        total = total + #chunk
    end
    fin:close()
    fout:close()
    return total
end

function setup()
    if #args < 2 then
        UtilityResult(false, "USAGE: COPY <FROM>... <TO|DIR>")
        return
    end
    local target = card_path(args[#args])
    local info = fs.stat(target)
    local into_dir = info and info.dir
    if #args > 2 and not into_dir then
        UtilityResult(false, "COPYING SEVERAL FILES NEEDS A DIRECTORY")
        return
    end
    local files, bytes, lines = 0, 0, {}
    for i = 1, #args - 1 do
        local source = card_path(args[i])
        local sinfo = fs.stat(source)
        if not sinfo then
            lines[#lines + 1] = "?NOT FOUND: " .. shown(source)
        elseif sinfo.dir then
            lines[#lines + 1] = "?SKIPPED DIRECTORY " .. shown(source)
        else
            local dest = into_dir and (target .. "/" .. base_name(source)) or target
            local n, err = copy_file(source, dest)
            if n then
                files, bytes = files + 1, bytes + n
            else
                lines[#lines + 1] = "?" .. shown(source) .. ": " .. tostring(err)
            end
        end
    end
    UtilityResult(files > 0 or #lines == 0, {
        message = Text.Plural(files, "FILE", "FILES") .. " COPIED, " .. Text.Commas(bytes) .. " BYTES",
        lines = lines,
    })
end

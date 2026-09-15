-- Entry points: open the file, index it over a few ticks, draw the
-- first page as soon as it is known.

-- A message box instead of the file; any key leaves.
local function fail(message, name)
    indexing = false
    Screen.Window(4, 10, 35, 17, "VIEW", Screen.DOUBLE, STATUS_ATTR)
    Screen.OutText(6, 12, message:sub(1, 28), STATUS_ATTR)
    if name then Screen.OutText(6, 13, name:gsub("^/data", ""):sub(1, 28), STATUS_ATTR) end
    Screen.OutText(6, 15, "press any key", STATUS_ATTR)
    on_keypress = function() ExitProgram() end
end

function setup()
    Screen.Mode(1)
    if not path or path == "" then
        fail("usage: view <file>")
        return
    end
    if path:sub(1, 1) ~= "/" then path = "/data/" .. path end
    local info, err = fs.stat(path)
    if not info or info.dir then
        fail(info and "that is a folder:" or "file not found:", path)
        return
    end
    file, err = fs.open(path, "r")
    if not file then
        fail(tostring(err), path)
        return
    end
    size = info.size
    if size > 0 then line_start_seen(0) end
    draw_title()
    draw_status()
    index_some(40)
    if lines >= H or not indexing then
        draw_page()
        drawn = true
    end
    on_keypress = view_key
end

-- Index in 40 ms slices so a big file never holds a tick for long; the
-- title shows "+" and "..." until the count is final.
function tick()
    if not indexing or not file then return end
    index_some(40)
    if not drawn and (lines >= H or not indexing) then
        draw_page()
        drawn = true
    else
        draw_title()
    end
    if not indexing and truncated then
        draw_status("showing the first " .. lines .. " lines")
    end
end

function finish()
    if file then file:close() end
end

-- Entry points.

function setup()
    Screen.Mode(1)
    -- `files <folder>` starts there (the shell passes /data/... paths).
    local start = args[1]
    if start then
        if start:sub(1, 1) ~= "/" then start = ROOT .. "/" .. start end
        local info = fs.stat(start)
        if info and info.dir and start:sub(1, #ROOT) == ROOT then cwd = start:gsub("/$", "") end
    end
    draw_frame()
    refresh()
    on_keypress = files_key
end

-- tick() runs only while this app is on top, so the first tick after a
-- viewer or editor exits is the moment to re-read the folder.
function tick()
    if child_running then
        child_running = false
        refresh(names[sel])
    end
end

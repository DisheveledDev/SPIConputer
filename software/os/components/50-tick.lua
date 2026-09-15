-- Main loop: blink the cursor, run the input line, repaint after a
-- launched program returns.

function tick()
    -- A utility's result: a message, or a table whose `message` field
    -- prints first, then its `lines` array in order, then every other
    -- field as a KEY = VALUE line.
    local utility_ok, utility_result = UtilityPoll()
    if utility_ok ~= nil then
        local prefix = utility_ok and "" or "?"
        if type(utility_result) == "table" then
            if utility_result.message ~= nil then
                out(prefix .. tostring(utility_result.message))
            end
            if type(utility_result.lines) == "table" then
                for _, line in ipairs(utility_result.lines) do
                    out(tostring(line))
                end
            end
            local keys = {}
            for key in pairs(utility_result) do
                if key ~= "message" and key ~= "lines" then keys[#keys + 1] = key end
            end
            table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
            for _, key in ipairs(keys) do
                out(string.format("%s = %s", tostring(key):upper(), tostring(utility_result[key])))
            end
        else
            out(prefix .. tostring(utility_result))
        end
        needs_repaint = true
    end
    if needs_repaint then
        needs_repaint = false
        paint()
        out("READY.")
    end

    -- The APPS picker takes every key while it is open; the prompt and
    -- its cursor blink wait (a blink would clear the overlay it lives on).
    if dialog_open then
        while true do
            local event = InputPoll()
            if not event then
                break
            end
            if event.type == "key" and event.pressed == 1 then
                dialog_key(event.key)
                if not dialog_open then
                    break
                end
            end
        end
        return
    end

    local now = TimeNow()
    if now - last_blink >= 500 then
        last_blink = now
        cursor_on = not cursor_on
        paint_cursor()
    end

    local changed = false
    while true do
        local event = InputPoll()
        if not event then
            break
        end
        if event.type == "key" and event.pressed == 1 then
            if event.key == KEY_RETURN then
                submit()
            else
                keyboard(event)
            end
            changed = true
        end
    end
    -- A command may have opened the APPS picker (leave its overlay alone)
    -- or launched a program whose setup() has already drawn its screen
    -- (needs_repaint: the shell repaints when it is back on top).
    if changed and not dialog_open and not needs_repaint then
        cursor_on = true
        paint()
    end
end

function finish()
end

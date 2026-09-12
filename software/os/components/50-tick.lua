-- Main loop: blink the cursor, run the input line, repaint after a
-- launched program returns.

function tick()
    local utility_ok, utility_message = UtilityPoll()
    if utility_ok ~= nil then
        out((utility_ok and "" or "?") .. utility_message)
        needs_repaint = true
    end
    if needs_repaint then
        needs_repaint = false
        paint()
        out("READY.")
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
    if changed then
        cursor_on = true
        paint()
    end
end

function finish()
end

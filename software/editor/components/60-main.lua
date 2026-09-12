-- Entry points: load the file, blink the cursor, pump input.

function setup()
    ScreenMode(1)
    ScreenPalette(0, 0, 0, 160)
    ScreenPalette(1, 255, 255, 255)
    local content, err = fs.readall(filename)
    if content then
        lines = load_content(content)
    else
        lines = { "" }
        print("new file: " .. tostring(err))
    end
    TimerCreate(function()
        cursor_on = not cursor_on
        draw()
    end, 500)
    draw()
end

function tick()
    while true do
        local ev = InputPoll()
        if not ev then break end
        if ev.type == "key" and ev.pressed == 1 then handle_key(ev) end
    end
end

-- Entry points: load the file, start the cursor blink, draw.

function setup()
    Screen.Mode(1)
    Screen.Palette(0, 0, 0, 160)
    Screen.Palette(1, 255, 255, 255)
    local content, err = fs.readall(filename)
    if content then
        lines = load_content(content)
    else
        lines = { "" }
        print("new file: " .. tostring(err))
    end
    -- The blink is two attribute ops every half second; menus pause it.
    blink_timer = Timer.Every(500, function()
        if not blink_enabled then return end
        cursor_visible = not cursor_visible
        draw_cursor()
    end)
    draw()
end

function tick()
end

function finish()
    Timer.CancelAll()
end

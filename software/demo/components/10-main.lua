local results = {}

local function check(name, ok, err)
    if ok then
        results[#results + 1] = "ok " .. name
    else
        results[#results + 1] = "FAIL " .. name .. ": " .. tostring(err)
    end
    return ok
end

local function save()
    fs.writeall("/data/demo-selftest.txt", table.concat(results, "\n") .. "\n")
end

function setup()
    check("mode", Screen.Mode(1))
    check("window", Screen.Window(0, 0, 39, 29, "SDK DEMO", Screen.DOUBLE))
    check("center", Screen.CenterText(2, "Screen.CenterText(2, ...)"))
    check("right", Screen.RightText(3, "RightText " .. string.char(16), 0x02))
    check("outtext", Screen.OutText(2, 5, "Fill, then Move to the right:", 0x02))
    check("fill", Screen.Fill(2, 7, 11, 9, 219))
    check("move", Screen.Move(2, 7, 11, 9, 24, 7))
    check("outtext2", Screen.OutText(2, 11, "Scroll region down 1:", 0x02))
    check("fill2", Screen.Fill(2, 13, 37, 13, 177))
    check("scroll", Screen.Scroll(2, 13, 37, 15, 0, 1))
    check("hline", Screen.HLine(2, 37, 17))
    check("vline", Screen.VLine(20, 18, 21))
    check("printf", Screen.Printf(2, 19, "Printf: %d + %d = %d", 2, 3, 2 + 3))
    check("clean", Screen.Clean(22, 18, 37, 21))
    check("box", Screen.Box(22, 18, 37, 21, Screen.SINGLE, 0x03))
    check("scrollup", Screen.ScrollUp(0))
    local ok, err = Screen.LoadImage("logo.bmp", 0, 0)
    check("loadimage-reserved", ok == nil and err ~= nil, "expected nil, err")
    local x1, y1, x2, y2 = Overlay.Dialog("HELLO", { "Overlay.Dialog over the screen", "press any key" })
    check("dialog", x1 ~= nil and x2 > x1 and y2 > y1, "no corners")
    check("overlay-text", Overlay.OutText(x1 + 2, y2 - 1, "[ OK ]", Overlay.INVERT))
    local voice = Sound.Beep()
    check("beep", type(voice) == "number", tostring(voice))
    check("tone", type(Sound.Tone(440, 50, 128)) == "number", "no voice")
    check("keyname", Input.Keyboard.Name(Input.KEY_RETURN) == "RETURN", Input.Keyboard.Name(13))
    Input.Keyboard.Callback("any", function(key)
        results[#results + 1] = "key " .. Input.Keyboard.Name(key)
        save()
        ExitProgram()
    end)
    Input.Joystick.Callback(1, "fire", function(port, action, pressed)
        results[#results + 1] = "joystick " .. port .. " " .. action .. " " .. tostring(pressed)
    end)
    save()
end

function tick()
end

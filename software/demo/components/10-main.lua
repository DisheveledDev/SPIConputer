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
    -- Text helpers.
    check("text-center", Text.Center("ab", 6) == "  ab  ", Text.Center("ab", 6))
    check("text-right", Text.Right("7", 3, "0") == "007", Text.Right("7", 3, "0"))
    check("text-truncate", Text.Truncate("hello world", 8) == "hello ..", Text.Truncate("hello world", 8))
    local wrapped = Text.Wrap("the quick brown fox jumps", 10)
    check("text-wrap", #wrapped == 3 and wrapped[1] == "the quick" and wrapped[3] == "jumps", table.concat(wrapped, "|"))
    check("text-commas", Text.Commas(1234567) == "1,234,567", Text.Commas(1234567))
    check("text-plural", Text.Plural(3, "file") == "3 files", Text.Plural(3, "file"))
    check("outwrapped", Screen.OutWrapped(2, 24, 20, "Screen.OutWrapped wraps this text to twenty cells") == 3, "rows")
    check("label", Screen.Label(24, 24, 14, "right", "right", 0x02))
    check("progress", Screen.Progress(24, 26, 14, 0.6))
    check("line", Screen.Line(2, 27, 20, 28, 250))
    check("attributes", Attributes.Red + Attributes.Inverse == 0x81, Attributes.Red + Attributes.Inverse)
    check("outattrs", Screen.OutAttrs(24, 28, string.char(Attributes.Red, Attributes.Green + Attributes.Inverse)))

    -- Timers: a one-shot fires, a paused timer does not tick while
    -- paused and resumes, a cancelled timer never fires. Checked by a
    -- final timer, which also writes the results.
    local fired, ticks = false, 0
    Timer.After(100, function() fired = true end)
    local every = Timer.Every(50, function(t) ticks = ticks + 1 end)
    local never = Timer.After(150, function() results[#results + 1] = "FAIL cancelled timer fired" end)
    check("timer-cancel", never:Cancel() and not Timer.Running(never))
    check("timer-count", Timer.Count() == 2, Timer.Count())
    Timer.After(200, function()
        local before = ticks
        check("timer-pause", every:Pause() and Timer.Paused(every) and not every:Running())
        Timer.After(200, function()
            check("timer-paused-holds", ticks == before, ticks .. " vs " .. before)
            check("timer-resume", Timer.Resume(every.id) and every:Running())
            Timer.After(200, function()
                check("timer-oneshot-fired", fired)
                check("timer-resumed-ticks", ticks > before, ticks .. " vs " .. before)
                check("timer-cancel-every", Timer.Cancel(every))
                check("timer-none-left", Timer.Count() == 0, Timer.Count())
                save()
            end)
        end)
    end)

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

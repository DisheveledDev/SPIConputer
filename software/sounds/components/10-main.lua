local presets = {}
local selected = 1
local scroll = 0
local LIST_TOP, LIST_ROWS = 2, 24
local playing = false            -- the MML tune

-- The MODULES page: every .mod in the app's resources folder.
local page = "sounds"            -- "sounds" or "modules"
local modules = {}               -- { name = "entity.mod", path = ... }
local mod_selected = 1
local module = nil               -- the loaded Music.LoadMod object
local mod_name = nil
local last_position = ""

local TUNE = {
    tempo = 132, loop = true,
    channels = {
        { sound = "lead",  mml = "o5 l8 e e r e r c e4 g4 r4 < g4 r4 > [ c4 < g r > e r a r b r a# a r ]2" },
        { sound = "bass",  mml = "o2 l4 [ c c g g a a g2 ]2 [ f f c c d d g2 ]2" },
        { sound = "kick",  mml = "l4 [ c r ]16" },
        { sound = "hihat", mml = "l8 [ r c ]32" },
    },
}

-- ---------------------------------------------------------------- sounds

local function draw_entry(index)
    local slot = index - scroll
    if slot < 1 or slot > LIST_ROWS then return end
    local p = presets[index]
    local y = LIST_TOP + slot - 1
    local attr = index == selected and Attributes.Yellow + Attributes.Inverse or
        (p.effect and Attributes.Cyan or Attributes.White)
    Screen.Label(0, y, 20, (index == selected and string.char(16) or " ") .. " " .. p.name:upper()
        .. (p.effect and "" or "  (inst)"), "left", attr)
end

local function draw_spec()
    local p = presets[selected]
    local spec = Sound.Spec(p.name)
    Screen.Clean(21, LIST_TOP, 39, LIST_TOP + LIST_ROWS - 1)
    local rows = {
        "wave     " .. spec.wave,
        "duty     " .. spec.duty,
        "attack   " .. spec.attack,
        "decay    " .. spec.decay,
        "sustain  " .. spec.sustain,
        "release  " .. spec.release,
        "volume   " .. spec.volume,
        "note     " .. (spec.note or "(as played)"),
        "slide    " .. spec.slide,
        "vibrato  " .. spec.vibrato .. " @" .. spec.vibrato_rate,
        "arp      " .. spec.arp .. "/" .. spec.arp2 .. " " .. spec.arp_ms .. "ms" .. (spec.arp_loop and " loop" or ""),
        "cutoff   " .. spec.cutoff,
    }
    Screen.OutText(21, LIST_TOP, (p.effect and "EFFECT " or "INSTRUMENT ") .. p.name:upper(), Attributes.Green)
    for i, row in ipairs(rows) do
        Screen.OutText(21, LIST_TOP + 1 + i, row)
    end
end

-- ---------------------------------------------------------------- modules

local function scan_modules()
    modules = {}
    for _, entry in ipairs(fs.ls(app.resources) or {}) do
        if not entry.dir and entry.name:lower():match("%.mod$") then
            modules[#modules + 1] = { name = entry.name, size = entry.size,
                                      path = app.resources .. "/" .. entry.name }
        end
    end
    table.sort(modules, function(a, b) return a.name:lower() < b.name:lower() end)
end

local function draw_module_entry(index)
    local m = modules[index]
    if not m or index > LIST_ROWS then return end
    local y = LIST_TOP + index - 1
    local current = m.name == mod_name
    local attr = index == mod_selected and Attributes.Yellow + Attributes.Inverse
        or (current and Attributes.Green or Attributes.White)
    Screen.Label(0, y, 20, (index == mod_selected and string.char(16) or (current and string.char(16) or " "))
        .. " " .. m.name:gsub("%.[Mm][Oo][Dd]$", ""):sub(1, 17), "left", attr)
end

-- The right panel: the loaded module's facts, and where it is.
local function draw_module_info()
    Screen.Clean(21, LIST_TOP, 39, LIST_TOP + LIST_ROWS - 1)
    local m = modules[mod_selected]
    if m then
        Screen.OutText(21, LIST_TOP, "FILE", Attributes.Green)
        Screen.OutText(21, LIST_TOP + 1, m.name:sub(1, 19))
        Screen.OutText(21, LIST_TOP + 2, (m.size // 1024) .. " KB")
    end
    local info = module and Music.ModInfo()
    if not info then
        Screen.OutText(21, LIST_TOP + 4, "RETURN plays it", Attributes.Cyan)
        return
    end
    Screen.OutText(21, LIST_TOP + 4, "PLAYING", Attributes.Green)
    Screen.OutText(21, LIST_TOP + 5, info.name:sub(1, 19))
    Screen.OutText(21, LIST_TOP + 6, "orders   " .. info.orders)
    Screen.OutText(21, LIST_TOP + 7, "patterns " .. info.patterns)
    Screen.OutText(21, LIST_TOP + 8, "samples  " .. info.samples)
    Screen.OutText(21, LIST_TOP + 9, "resident " .. info.resident_kb .. " KB")
    Screen.OutText(21, LIST_TOP + 11, "underruns " .. info.underruns,
                   info.underruns > 0 and Attributes.Red or Attributes.White)
end

local function draw_position()
    if not module then return end
    local order, row = Music.ModPosition()
    local info = Music.ModInfo()
    local text = string.format("pos %2d/%2d row %2d", order or 0, (info and info.orders or 0) - 1, row or 0)
    if text ~= last_position then
        last_position = text
        Screen.OutText(21, LIST_TOP + 12, text, Attributes.Yellow)
        Screen.OutText(21, LIST_TOP + 11, "underruns " .. (info and info.underruns or 0) .. "   ",
                       info and info.underruns > 0 and Attributes.Red or Attributes.White)
    end
end

-- ---------------------------------------------------------------- screen

local function draw_all()
    Screen.Clear()
    if page == "sounds" then
        Screen.Label(0, 0, 40, "BUILT-IN SOUNDS  " .. #presets .. "   TAB: modules", "center", Attributes.Inverse)
        for i = scroll + 1, scroll + LIST_ROWS do draw_entry(i) end
        draw_spec()
        Screen.Label(0, 27, 40, "RETURN play  LEFT/RIGHT -5/+7", "center", Attributes.Green)
        Screen.Label(0, 28, 40, "M tune" .. (playing and "*" or "") .. "  ESC quit", "center", Attributes.Green)
    else
        Screen.Label(0, 0, 40, "MODULES  " .. #modules .. "   TAB: sounds", "center", Attributes.Inverse)
        if #modules == 0 then
            Screen.OutText(1, LIST_TOP, "no .mod files in resources")
        end
        for i = 1, #modules do draw_module_entry(i) end
        draw_module_info()
        last_position = ""
        draw_position()
        Screen.Label(0, 27, 40, "RETURN play  SPACE stop", "center", Attributes.Green)
        Screen.Label(0, 28, 40, "streamed from the card  ESC quit", "center", Attributes.Green)
    end
end

local function move_to(index)
    index = math.max(1, math.min(index, #presets))
    if index == selected then return end
    local previous = selected
    selected = index
    if selected <= scroll then scroll = selected - 1
    elseif selected > scroll + LIST_ROWS then scroll = selected - LIST_ROWS end
    if selected <= previous + 1 and selected >= previous - 1 and scroll == (selected <= scroll and selected - 1 or scroll) then
        draw_entry(previous)
        draw_entry(selected)
    else
        for i = scroll + 1, scroll + LIST_ROWS do draw_entry(i) end
    end
    draw_spec()
end

local function play(offset)
    local p = presets[selected]
    if p.effect then
        Sound.Effect(p.name)
    else
        Sound.Play(p.name, 60 + (offset or 0), 400)
    end
end

local function stop_module()
    if module then
        module:Stop()
    end
end

local function play_module()
    local m = modules[mod_selected]
    if not m then return end
    if playing then Music.Stop() playing = false end
    stop_module()
    module = nil
    mod_name = nil
    local loaded, err = Music.LoadMod(m.path)
    if not loaded then
        Screen.Label(0, 26, 40, "?" .. tostring(err), "center", Attributes.Red)
        return
    end
    module = loaded
    mod_name = m.name
    module:Play()
    draw_all()
end

function on_keypress(key)
    if key == Input.KEY_ESCAPE then
        ExitProgram()
    elseif key == Input.KEY_TAB then
        page = page == "sounds" and "modules" or "sounds"
        draw_all()
    elseif page == "modules" then
        if key == Input.KEY_UP or key == Input.KEY_DOWN then
            local previous = mod_selected
            mod_selected = math.max(1, math.min(mod_selected + (key == Input.KEY_UP and -1 or 1), #modules))
            draw_module_entry(previous)
            draw_module_entry(mod_selected)
            draw_module_info()
            last_position = ""
            draw_position()
        elseif key == Input.KEY_RETURN then
            play_module()
        elseif key == Input.KEY_SPACE then
            stop_module()
        end
    elseif key == Input.KEY_UP then move_to(selected - 1)
    elseif key == Input.KEY_DOWN then move_to(selected + 1)
    elseif key == Input.KEY_RETURN then play(0)
    elseif key == Input.KEY_LEFT then play(-5)
    elseif key == Input.KEY_RIGHT then play(7)
    elseif key == 109 or key == 77 then
        stop_module()
        if playing then Music.Stop() else Music.Play("demo") end
        playing = not playing
        draw_all()
    end
end

function setup()
    Screen.Mode(1)
    presets = Sound.Presets()
    scan_modules()
    Music.Track("demo", TUNE)
    draw_all()
end

local next_poll = 0

function tick()
    -- The module's position, ten times a second: one text op when it moved.
    if page == "modules" and module then
        local now = TimeNow()
        if now >= next_poll then
            next_poll = now + 100
            draw_position()
        end
    end
end

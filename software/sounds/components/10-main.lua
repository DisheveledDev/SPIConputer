local presets = {}
local selected = 1
local scroll = 0
local LIST_TOP, LIST_ROWS = 2, 24
local playing = false
local module, mod_playing = nil, false

local TUNE = {
    tempo = 132, loop = true,
    channels = {
        { sound = "lead",  mml = "o5 l8 e e r e r c e4 g4 r4 < g4 r4 > [ c4 < g r > e r a r b r a# a r ]2" },
        { sound = "bass",  mml = "o2 l4 [ c c g g a a g2 ]2 [ f f c c d d g2 ]2" },
        { sound = "kick",  mml = "l4 [ c r ]16" },
        { sound = "hihat", mml = "l8 [ r c ]32" },
    },
}

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

local function draw_all()
    Screen.Clear()
    Screen.Label(0, 0, 40, "BUILT-IN SOUNDS  " .. #presets, "center", Attributes.Inverse)
    for i = scroll + 1, scroll + LIST_ROWS do draw_entry(i) end
    draw_spec()
    Screen.Label(0, 27, 40, "RETURN play  LEFT/RIGHT -5/+7", "center", Attributes.LightGreen or Attributes.Green)
    Screen.Label(0, 28, 40, "M tune" .. (playing and "*" or "") .. "  D mod" .. (mod_playing and "*" or "") .. "  ESC quit", "center", Attributes.Green)
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

function on_keypress(key)
    if key == Input.KEY_ESCAPE then
        ExitProgram()
    elseif key == Input.KEY_UP then move_to(selected - 1)
    elseif key == Input.KEY_DOWN then move_to(selected + 1)
    elseif key == Input.KEY_RETURN then play(0)
    elseif key == Input.KEY_LEFT then play(-5)
    elseif key == Input.KEY_RIGHT then play(7)
    elseif key == 109 or key == 77 then
        if mod_playing then Music.StopMod() mod_playing = false end
        if playing then Music.Stop() else Music.Play("demo") end
        playing = not playing
        draw_all()
    elseif key == 100 or key == 68 then
        -- D: the bundled ProTracker module, streamed from the card.
        if playing then Music.Stop() playing = false end
        if mod_playing then
            Music.StopMod()
        elseif not module then
            module = Music.LoadMod(app.resources .. "/demo.mod")
            if module then module:Play() end
        else
            module:Play()
        end
        mod_playing = module ~= nil and not mod_playing
        draw_all()
    end
end

function setup()
    Screen.Mode(1)
    presets = Sound.Presets()
    Music.Track("demo", TUNE)
    draw_all()
end

function tick()
end

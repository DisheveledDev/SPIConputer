local page = "modules"           -- "modules", "tracker" or "sounds"

-- ---------------------------------------------------------------- modules

local modules = {}               -- { name = "enjoy.mod", size = bytes, path = ... }
local mod_selected = 1
local module = nil               -- the Music.LoadMod object playing (or stopped)
local mod_index = 0              -- which of `modules` it is
local mod_info, samples = nil, nil

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

-- ---------------------------------------------------------------- tracker layout (80x60)

local BAR_X, BAR_TOP, BAR_ROWS = 1, 3, 20       -- analyser bars: rows 3..22
local BAR_W, BAR_STRIDE = 4, 5                  -- 10 bands, 4 cells wide
local BANDS = 10
local CH_X, CH_TOP = 53, 3                      -- channel panel: 4 x 5 rows
local VU_W = 26
local PAT_X, PAT_TOP, PAT_ROWS = 16, 27, 25     -- pattern rows 27..51
local PAT_HALF = 12                             -- rows above the one playing
local PAT_W = 47
local PROGRESS_Y, STATUS_Y, HELP_Y = 54, 55, 58

local CH_COLOUR = { Attributes.Cyan, Attributes.Green, Attributes.Yellow, Attributes.Purple }
local INV = Attributes.Inverse

-- Incremental drawing state.
local levels = {}                -- Sound.Spectrum, refilled in place
local chans = {}                 -- Music.ModChannels, refilled in place
local bar_h, bar_peak, bar_hold, bar_mark = {}, {}, {}, {}
local vu, ch_text = {}, {}
local last_order, last_row, last_pattern = -1, -1, -1
local last_status = ""
local next_frame = 0

local function reset_visuals()
    for b = 1, BANDS do bar_h[b], bar_peak[b], bar_hold[b], bar_mark[b] = 0, 0, 0, -1 end
    for c = 1, 4 do vu[c], ch_text[c] = 0, "" end
    last_order, last_row, last_pattern, last_status = -1, -1, -1, ""
end

-- One analyser cell: cell j (0 = bottom) of band b for a bar `h` half-steps
-- tall: a block, a half block or blank, coloured by height.
local function bar_cell_attr(j)
    if j >= 15 then return Attributes.Red elseif j >= 9 then return Attributes.Yellow end
    return Attributes.Green
end

local function draw_bar_cell(b, j, h)
    local x = BAR_X + (b - 1) * BAR_STRIDE
    local y = BAR_TOP + BAR_ROWS - 1 - j
    local char = 32
    if h >= 2 * (j + 1) then char = 219 elseif h == 2 * j + 1 then char = 220 end
    Screen.Fill(x, y, x + BAR_W - 1, y, char, bar_cell_attr(j))
end

-- Redraw the cells a band's bar changes between heights old and h, and
-- move its peak marker (a line the cell above the bar's highest point,
-- falling slowly).
local function draw_bar(b, h, peak)
    local old = bar_h[b]
    if h ~= old then
        local lo, hi = math.min(old, h), math.max(old, h)
        for j = lo // 2, math.min(BAR_ROWS - 1, (hi - 1) // 2) do
            draw_bar_cell(b, j, h)
        end
        bar_h[b] = h
    end
    local mark = (peak + 1) // 2           -- the cell above the peak's top
    if mark >= BAR_ROWS or peak == 0 then mark = -1 end
    if mark ~= bar_mark[b] then
        if bar_mark[b] >= 0 then draw_bar_cell(b, bar_mark[b], h) end
        if mark >= 0 then
            local x = BAR_X + (b - 1) * BAR_STRIDE
            local y = BAR_TOP + BAR_ROWS - 1 - mark
            Screen.Fill(x, y, x + BAR_W - 1, y, 196, Attributes.White)
        end
        bar_mark[b] = mark
    end
end

local function update_analyser()
    Sound.Spectrum(levels)
    for b = 1, BANDS do
        local target = levels[b] * (2 * BAR_ROWS) // 255
        local h = bar_h[b]
        if target >= h then h = target else h = math.max(target, h - 3) end
        local peak = bar_peak[b]
        if h >= peak then
            peak, bar_hold[b] = h, 24
        elseif bar_hold[b] > 0 then
            bar_hold[b] = bar_hold[b] - 1
        elseif peak > 0 then
            peak = peak - 1
        end
        bar_peak[b] = peak
        draw_bar(b, h, peak)
    end
end

-- A channel's VU bar, redrawn only over the cells that change.
local function vu_attr(i)
    if i > 21 then return Attributes.Red elseif i > 15 then return Attributes.Yellow end
    return Attributes.Green
end

local function draw_vu(c, n)
    local old = vu[c]
    if n == old then return end
    local y = CH_TOP + (c - 1) * 5 + 1
    if n > old then
        local i = old + 1
        while i <= n do
            local stop = math.min(n, i > 21 and 26 or (i > 15 and 21 or 15))
            Screen.Fill(CH_X + i - 1, y, CH_X + stop - 1, y, 219, vu_attr(i))
            i = stop + 1
        end
    else
        Screen.Fill(CH_X + n, y, CH_X + old - 1, y, 176, Attributes.Blue)
    end
    vu[c] = n
end

local function update_channels()
    Music.ModChannels(chans)
    for c = 1, 4 do
        local ch = chans[c]
        local smp = ch.sample
        local name = smp > 0 and samples and samples[smp] and samples[smp].name or ""
        local text = string.format("%s %02X v%02d  %s", ch.active and ch.note or "...", smp, ch.volume, name)
        if text ~= ch_text[c] then
            ch_text[c] = text
            Screen.Label(CH_X, CH_TOP + (c - 1) * 5, VU_W, text, "left", CH_COLOUR[c])
        end
        draw_vu(c, math.min(VU_W, ch.level * VU_W // 200))
    end
end

-- The pattern view: the rows around the one playing, that one inverse,
-- every fourth row cyan. A one-row advance scrolls; anything else redraws.
local function pattern_attr(r, current)
    if current then return Attributes.Yellow + INV end
    return r % 4 == 0 and Attributes.Cyan or Attributes.White
end

local function pattern_text(r, cells)
    if r < 0 or r > 63 or not cells then return "" end
    return string.format("%02d  %s", r, cells)
end

local function draw_pattern(pattern, row)
    local rows = Music.ModRows(pattern, row - PAT_HALF, PAT_ROWS)
    for i = 1, PAT_ROWS do
        local r = row - PAT_HALF + i - 1
        Screen.Label(PAT_X, PAT_TOP + i - 1, PAT_W, pattern_text(r, rows and rows[i]), "left",
                     pattern_attr(r, i == PAT_HALF + 1))
    end
    return rows ~= nil
end

local function scroll_pattern(pattern, row)
    local bottom = PAT_TOP + PAT_ROWS - 1
    Screen.Scroll(PAT_X, PAT_TOP, PAT_X + PAT_W - 1, bottom, 0, -1)
    local y = PAT_TOP + PAT_HALF
    -- The row that was playing, back to normal; the new one, highlighted.
    local rows = Music.ModRows(pattern, row - 1, 2)
    Screen.Label(PAT_X, y - 1, PAT_W, pattern_text(row - 1, rows and rows[1]), "left", pattern_attr(row - 1, false))
    Screen.Label(PAT_X, y, PAT_W, pattern_text(row, rows and rows[2]), "left", pattern_attr(row, true))
    local r = row + PAT_ROWS - PAT_HALF - 1
    local tail = Music.ModRows(pattern, r, 1)
    Screen.Label(PAT_X, bottom, PAT_W, pattern_text(r, tail and tail[1]), "left", pattern_attr(r, false))
    return rows ~= nil
end

local function draw_status(order, row)
    local info = Music.ModInfo()
    local text = string.format("order %02d/%02d   row %02d   underruns %d", order, (mod_info and mod_info.orders or 1) - 1, row,
                               info and info.underruns or 0)
    if text ~= last_status then
        last_status = text
        Screen.Label(0, STATUS_Y, 80, text, "center", info and info.underruns > 0 and Attributes.Red or Attributes.Green)
    end
    if mod_info and mod_info.orders > 0 then
        Screen.Progress(1, PROGRESS_Y, 78, (order + row / 64) / mod_info.orders, Attributes.Cyan)
    end
end

local function update_position()
    local order, row, pattern = Music.ModPosition()
    if not order then return end
    if order == last_order and row == last_row and pattern == last_pattern then return end
    local ok
    if pattern == last_pattern and row == last_row + 1 then
        ok = scroll_pattern(pattern, row)
    else
        ok = draw_pattern(pattern, row)
    end
    draw_status(order, row)
    if ok then
        last_order, last_row, last_pattern = order, row, pattern
    else
        last_row = -1 -- the pattern was not in RAM yet: redraw next time
    end
end

local function draw_tracker()
    Screen.Clear()
    reset_visuals()
    local name = mod_info and mod_info.name ~= "" and mod_info.name or (modules[mod_index] and modules[mod_index].name) or ""
    Screen.Label(0, 0, 80, "SPITRACKER   " .. name:upper(), "center", INV)
    -- Analyser frame and band labels.
    local hz = Sound.SpectrumBands()
    for b = 1, BANDS do
        local x = BAR_X + (b - 1) * BAR_STRIDE
        local f = hz[b]
        local label = f >= 1000 and string.format("%dk%s", f // 1000, (f % 1000 ~= 0) and tostring((f % 1000) // 100) or "") or tostring(f)
        Screen.Label(x, BAR_TOP + BAR_ROWS, BAR_W, label, "center", Attributes.Blue)
    end
    Screen.OutText(BAR_X, BAR_TOP - 1, "SPECTRUM", Attributes.Blue)
    Screen.OutText(CH_X, CH_TOP - 1, "CHANNELS", Attributes.Blue)
    for c = 1, 4 do
        local y = CH_TOP + (c - 1) * 5
        Screen.OutText(CH_X, y + 2, "CH" .. c, CH_COLOUR[c])
        Screen.Fill(CH_X, y + 1, CH_X + VU_W - 1, y + 1, 176, Attributes.Blue)
    end
    Screen.Label(PAT_X, PAT_TOP - 1, PAT_W, "ROW CH1        CH2        CH3        CH4", "left", Attributes.Blue)
    Screen.Label(0, HELP_Y, 80, "SPACE stop/play   N/P next/prev module   TAB modules   F1 sounds   ESC back", "center", Attributes.Green)
    update_position()
    update_channels()
end

-- ---------------------------------------------------------------- modules page

local function draw_module_entry(index)
    local m = modules[index]
    if not m or index > 50 then return end
    local current = index == mod_index
    local attr = index == mod_selected and Attributes.Yellow + INV or (current and Attributes.Green or Attributes.White)
    local mark = (index == mod_selected or current) and string.char(16) or " "
    Screen.Label(20, 3 + index, 40, string.format("%s %-28s %5d KB", mark, m.name:sub(1, 28), m.size // 1024), "left", attr)
end

local function draw_modules()
    Screen.Clear()
    Screen.Label(0, 0, 80, "SPITRACKER   MODULES  " .. #modules, "center", INV)
    Screen.OutText(20, 2, "the .mod files in the app's resources folder", Attributes.Blue)
    if #modules == 0 then Screen.OutText(22, 4, "none found", Attributes.Red) end
    for i = 1, #modules do draw_module_entry(i) end
    Screen.Label(0, HELP_Y, 80, "UP/DOWN choose   RETURN play   TAB tracker   F1 sounds   ESC quit", "center", Attributes.Green)
end

local function stop_module()
    if module then module:Stop() end
end

local function play_module(index)
    local m = modules[index]
    if not m then return false end
    stop_module()
    module, mod_info, samples, mod_index = nil, nil, nil, 0
    local loaded, err = Music.LoadMod(m.path)
    if not loaded then
        Screen.Label(0, HELP_Y - 2, 80, "? " .. tostring(err), "center", Attributes.Red)
        return false
    end
    module, mod_index = loaded, index
    mod_info = module:Info()
    samples = module:Samples()
    module:Play()
    return true
end

local function play_relative(step)
    if #modules == 0 then return end
    local index = mod_index + step
    if index < 1 then index = #modules elseif index > #modules then index = 1 end
    mod_selected = index
    if play_module(index) then
        page = "tracker"
        draw_tracker()
    end
end

-- ---------------------------------------------------------------- sounds page

local presets, selected, scroll = {}, 1, 0
local LIST_TOP, LIST_ROWS, SPEC_X = 3, 50, 30

local function draw_entry(index)
    local slot = index - scroll
    if slot < 1 or slot > LIST_ROWS then return end
    local p = presets[index]
    if not p then return end
    local attr = index == selected and Attributes.Yellow + INV or (p.effect and Attributes.Cyan or Attributes.White)
    Screen.Label(2, LIST_TOP + slot - 1, 24, (index == selected and string.char(16) or " ") .. " " .. p.name:upper()
        .. (p.effect and "" or "  (inst)"), "left", attr)
end

local function draw_spec()
    local p = presets[selected]
    if not p then return end
    local spec = Sound.Spec(p.name)
    Screen.Clean(SPEC_X, LIST_TOP, 79, LIST_TOP + 14)
    Screen.OutText(SPEC_X, LIST_TOP, (p.effect and "EFFECT " or "INSTRUMENT ") .. p.name:upper(), Attributes.Green)
    local rows = {
        "wave     " .. spec.wave, "duty     " .. spec.duty, "attack   " .. spec.attack,
        "decay    " .. spec.decay, "sustain  " .. spec.sustain, "release  " .. spec.release,
        "volume   " .. spec.volume, "note     " .. (spec.note or "(as played)"),
        "slide    " .. spec.slide, "vibrato  " .. spec.vibrato .. " @" .. spec.vibrato_rate,
        "arp      " .. spec.arp .. "/" .. spec.arp2 .. " " .. spec.arp_ms .. "ms" .. (spec.arp_loop and " loop" or ""),
        "cutoff   " .. spec.cutoff,
    }
    for i, row in ipairs(rows) do Screen.OutText(SPEC_X, LIST_TOP + 1 + i, row) end
end

local function draw_sounds()
    Screen.Clear()
    Screen.Label(0, 0, 80, "SPITRACKER   BUILT-IN SOUNDS  " .. #presets, "center", INV)
    for i = scroll + 1, scroll + LIST_ROWS do draw_entry(i) end
    draw_spec()
    Screen.Label(0, HELP_Y, 80, "UP/DOWN choose   RETURN play   LEFT/RIGHT -5/+7 semitones   ESC back", "center", Attributes.Green)
end

local function move_to(index)
    index = math.max(1, math.min(index, #presets))
    if index == selected then return end
    local previous = selected
    selected = index
    if selected <= scroll then scroll = selected - 1
    elseif selected > scroll + LIST_ROWS then scroll = selected - LIST_ROWS end
    draw_entry(previous)
    draw_entry(selected)
    draw_spec()
end

local function play_preset(offset)
    local p = presets[selected]
    if not p then return end
    if p.effect then Sound.Effect(p.name) else Sound.Play(p.name, 60 + (offset or 0), 400) end
end

-- ---------------------------------------------------------------- keys

local function show(new_page)
    page = new_page
    if page == "tracker" then draw_tracker()
    elseif page == "sounds" then draw_sounds()
    else draw_modules() end
end

function on_keypress(key)
    if key == Input.KEY_F1 then
        show(page == "sounds" and (module and "tracker" or "modules") or "sounds")
    elseif key == Input.KEY_TAB then
        show(page == "tracker" and "modules" or (module and "tracker" or "modules"))
    elseif key == Input.KEY_ESCAPE then
        if page == "modules" then ExitProgram() else show("modules") end
    elseif page == "modules" then
        if key == Input.KEY_UP or key == Input.KEY_DOWN then
            local previous = mod_selected
            mod_selected = math.max(1, math.min(mod_selected + (key == Input.KEY_UP and -1 or 1), #modules))
            draw_module_entry(previous)
            draw_module_entry(mod_selected)
        elseif key == Input.KEY_RETURN then
            if play_module(mod_selected) then show("tracker") end
        elseif key == Input.KEY_SPACE then
            stop_module()
        end
    elseif page == "tracker" then
        if key == Input.KEY_SPACE then
            if module:Playing() then stop_module() else module:Play() last_row = -1 end
        elseif key == 110 or key == 78 or key == Input.KEY_RIGHT then play_relative(1)   -- N
        elseif key == 112 or key == 80 or key == Input.KEY_LEFT then play_relative(-1)   -- P
        end
    else
        if key == Input.KEY_UP then move_to(selected - 1)
        elseif key == Input.KEY_DOWN then move_to(selected + 1)
        elseif key == Input.KEY_RETURN then play_preset(0)
        elseif key == Input.KEY_LEFT then play_preset(-5)
        elseif key == Input.KEY_RIGHT then play_preset(7)
        end
    end
end

function setup()
    Screen.Mode(Screen.TEXT80C)
    presets = Sound.Presets()
    scan_modules()
    reset_visuals()
    draw_modules()
end

function tick()
    if page ~= "tracker" or not module then return end
    local now = TimeNow()
    if now < next_frame then return end
    next_frame = now + 16
    update_position()
    update_channels()
    update_analyser()
end

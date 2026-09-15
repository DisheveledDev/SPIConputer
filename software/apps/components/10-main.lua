local COLS = Screen.COLS
local LIST_TOP = 3
local PAGE = 12                           -- entries shown: two rows each
local INNER_X, INNER_W = 1, COLS - 2
local FRAME = Attributes.Cyan
local SELECT = Attributes.Yellow + Attributes.Inverse
local GAME_TAG = Attributes.Green
local CH_POINTER, CH_UP, CH_DOWN = 16, 30, 31

local apps = {}
local selected = 1
local scroll = 0

-- One string field of a bundle's app.json (flat, generated JSON).
local function json_field(text, key)
    local s = text:find('"' .. key .. '"%s*:%s*"')
    if not s then return nil end
    local pos = text:find('"', text:find(":", s, true), true) + 1
    local parts = {}
    while pos <= #text do
        local c = text:sub(pos, pos)
        if c == '"' then return table.concat(parts) end
        if c == "\\" then
            local e = text:sub(pos + 1, pos + 1)
            parts[#parts + 1] = (e == "n" or e == "t") and " " or (e == "u" and "?" or e)
            pos = pos + (e == "u" and 6 or 2)
        else
            parts[#parts + 1] = c
            pos = pos + 1
        end
    end
    return nil
end

local function bundle_program(folder)
    local compiled = fs.find("app.prg", folder)
    if compiled then return folder .. "/" .. compiled end
    local source = fs.find("app.lua", folder)
    if source then return folder .. "/" .. source end
    return nil
end

local function scan(root, ext, kind)
    for _, entry in ipairs(fs.ls(root) or {}) do
        if entry.dir then
            local folder = root .. "/" .. entry.name
            local program = bundle_program(folder)
            if program then
                local meta = fs.readall(folder .. "/app.json") or ""
                apps[#apps + 1] = {
                    name = (json_field(meta, "name") or entry.name:gsub(ext .. "$", "")):upper(),
                    version = json_field(meta, "version") or "",
                    description = json_field(meta, "description") or "",
                    path = program,
                    kind = kind,
                }
            end
        end
    end
end

local function draw_entry(index)
    local slot = index - scroll
    if slot < 1 or slot > PAGE then return end
    local y = LIST_TOP + (slot - 1) * 2
    local app = apps[index]
    if not app then
        Screen.Clean(INNER_X, y, INNER_X + INNER_W - 1, y + 1)
        return
    end
    local chosen = index == selected
    local tag = app.kind == "game" and "GAME" or (app.version ~= "" and "V" .. app.version or "")
    local name = (chosen and string.char(CH_POINTER) or " ") .. " " .. app.name
    name = name:sub(1, INNER_W - #tag - 1)
    local attr = chosen and SELECT or 0
    Screen.Label(INNER_X, y, INNER_W, name .. string.rep(" ", INNER_W - #name - #tag) .. tag,
                 "left", attr)
    if not chosen and app.kind == "game" then
        Screen.OutText(INNER_X + INNER_W - #tag, y, tag, GAME_TAG)
    end
    Screen.Label(INNER_X, y + 1, INNER_W, "  " .. app.description, "left", attr)
end

local function draw_list()
    for i = scroll + 1, scroll + PAGE do draw_entry(i) end
    Screen.Out(COLS - 1, LIST_TOP, scroll > 0 and CH_UP or 186, FRAME)
    Screen.Out(COLS - 1, LIST_TOP + PAGE * 2 - 1,
               scroll + PAGE < #apps and CH_DOWN or 186, FRAME)
end

local function draw()
    Screen.Clear()
    Screen.Window(0, 0, COLS - 1, Screen.ROWS - 1, "APPS", Screen.DOUBLE, FRAME)
    Screen.Label(INNER_X, 1, INNER_W,
                 Text.Plural(#apps, "PROGRAM", "PROGRAMS") .. " INSTALLED", "center", 0)
    if #apps == 0 then
        Screen.CenterText(12, "NO APPS INSTALLED")
    else
        draw_list()
    end
    Screen.Label(INNER_X, Screen.ROWS - 2, INNER_W,
                 string.char(24, 25) .. " SELECT  RETURN RUN  ESC CLOSE", "center", FRAME)
end

local function move_to(index)
    index = math.max(1, math.min(index, #apps))
    if index == selected then return end
    local previous = selected
    selected = index
    local old_scroll = scroll
    if selected <= scroll then
        scroll = selected - 1
    elseif selected > scroll + PAGE then
        scroll = selected - PAGE
    end
    if scroll ~= old_scroll then
        draw_list()
    else
        draw_entry(previous)
        draw_entry(selected)
    end
end

function on_keypress(key)
    if key == Input.KEY_ESCAPE or key == Input.KEY_RUNSTOP or key == 113 or key == 81 then
        ExitProgram()
    elseif key == Input.KEY_RETURN then
        local app = apps[selected]
        if app then
            UtilityResult(true, { run = app.path, kind = app.kind })
        else
            ExitProgram()
        end
    elseif key == Input.KEY_UP then
        move_to(selected - 1)
    elseif key == Input.KEY_DOWN then
        move_to(selected + 1)
    elseif key == Input.KEY_HOME then
        move_to(1)
    end
end

function setup()
    Screen.Mode(1)
    scan("/apps", "%.app", "application")
    scan("/games", "%.game", "game")
    table.sort(apps, function(a, b) return a.name < b.name end)
    draw()
end

function tick()
end

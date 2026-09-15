-- APPS picker: a dialog on the overlay listing the installed apps by
-- name and description (from each bundle's app.json), scrolled with the
-- cursor keys. RETURN runs the selected app, ESC or RUN/STOP closes.
-- While it is open the shell's prompt, cursor blink and line editor
-- are paused (see tick).

local KEY_UP = 128
local KEY_DOWN = 129
local KEY_ESCAPE = 27
local KEY_RUNSTOP = 140

local CH_POINTER = 16    -- right-pointing pointer
local CH_UP = 30         -- up-pointing triangle
local CH_DOWN = 31       -- down-pointing triangle
local CH_ARROW_UP = 24
local CH_ARROW_DOWN = 25

-- Dialog geometry: a double frame just inside the screen, two rows per
-- app (name and version, then the description).
local DLG_X, DLG_Y, DLG_W, DLG_H = 1, 2, 38, 26
local LIST_TOP = DLG_Y + 2
local LIST_ROWS = DLG_H - 4
local PAGE = LIST_ROWS // 2
local BODY = 0x80        -- inverted: the dialog body
local HILITE = 0x00      -- plain: the selected app reads as a bar

local apps = {}          -- { name, version, description, path }
local selected = 1
local scroll = 0         -- index of the first visible app minus one

-- Minimal JSON reader for app.json (objects, arrays, strings, numbers,
-- true/false/null). Returns nil on malformed input.
local function json_decode(text)
    local pos = 1
    local value

    local function skip()
        pos = text:find("[^ \t\r\n]", pos) or #text + 1
    end

    local function str()
        local out = {}
        pos = pos + 1
        while pos <= #text do
            local c = text:sub(pos, pos)
            if c == '"' then
                pos = pos + 1
                return table.concat(out)
            elseif c == "\\" then
                local e = text:sub(pos + 1, pos + 1)
                local map = { n = "\n", t = "\t", r = "\r", b = "\b", f = "\f" }
                if e == "u" then
                    local code = tonumber(text:sub(pos + 2, pos + 5), 16) or 63
                    out[#out + 1] = code < 128 and string.char(code) or "?"
                    pos = pos + 6
                else
                    out[#out + 1] = map[e] or e
                    pos = pos + 2
                end
            else
                out[#out + 1] = c
                pos = pos + 1
            end
        end
        return nil
    end

    value = function()
        skip()
        local c = text:sub(pos, pos)
        if c == "{" then
            local obj = {}
            pos = pos + 1
            skip()
            if text:sub(pos, pos) == "}" then pos = pos + 1 return obj end
            while true do
                skip()
                if text:sub(pos, pos) ~= '"' then return nil end
                local key = str()
                if not key then return nil end
                skip()
                if text:sub(pos, pos) ~= ":" then return nil end
                pos = pos + 1
                local v = value()
                if v == nil and text:sub(pos - 4, pos - 1) ~= "null" then return nil end
                obj[key] = v
                skip()
                local sep = text:sub(pos, pos)
                pos = pos + 1
                if sep == "}" then return obj end
                if sep ~= "," then return nil end
            end
        elseif c == "[" then
            local arr = {}
            pos = pos + 1
            skip()
            if text:sub(pos, pos) == "]" then pos = pos + 1 return arr end
            while true do
                local v = value()
                arr[#arr + 1] = v
                skip()
                local sep = text:sub(pos, pos)
                pos = pos + 1
                if sep == "]" then return arr end
                if sep ~= "," then return nil end
            end
        elseif c == '"' then
            return str()
        elseif text:sub(pos, pos + 3) == "true" then
            pos = pos + 4
            return true
        elseif text:sub(pos, pos + 4) == "false" then
            pos = pos + 5
            return false
        elseif text:sub(pos, pos + 3) == "null" then
            pos = pos + 4
            return nil
        else
            local num = text:match("^-?%d+%.?%d*[eE]?[-+]?%d*", pos)
            if not num or num == "" then return nil end
            pos = pos + #num
            return tonumber(num)
        end
    end

    local ok, result = pcall(value)
    if ok then return result end
    return nil
end

-- Scan one root: bundles (folders with app.json) carry their metadata,
-- loose .prg/.lua programs are listed by file name.
local function scan_root(root, ext, kind, found)
    local entries = fs.ls(root)
    if not entries then
        return
    end
    for _, entry in ipairs(entries) do
        local lower = entry.name:lower()
        if entry.dir then
            local program = find_app_program(root, entry.name)
            if program then
                local meta = {}
                local text = fs.readall(root .. "/" .. entry.name .. "/app.json")
                if text then
                    meta = json_decode(text) or {}
                end
                found[#found + 1] = {
                    name = tostring(meta.name or entry.name:gsub(ext .. "$", "")),
                    version = meta.version and tostring(meta.version) or "",
                    description = tostring(meta.description or ""),
                    path = program,
                    kind = kind,
                }
            end
        elseif kind == "application" and (lower:match("%.prg$") or lower:match("%.lua$")) then
            found[#found + 1] = {
                name = entry.name:gsub("%.[^.]+$", ""),
                version = "",
                description = "",
                path = root .. "/" .. entry.name,
                kind = "program",
            }
        end
    end
end

-- Installed apps (/apps) and games (/games), sorted by name. A game
-- shows "GAME" in place of its version and takes the machine over when
-- run (see run_program).
local function load_apps()
    local found = {}
    scan_root("/apps", "%.app", "application", found)
    scan_root("/games", "%.game", "game", found)
    table.sort(found, function(a, b) return a.name:lower() < b.name:lower() end)
    return found
end

local function dialog_text(x, y, s, attr)
    for i = 1, math.min(#s, DLG_X + DLG_W - 1 - x) do
        OverlayOut(x + i - 1, y, s:byte(i), attr)
    end
end

local function draw_dialog()
    OverlayClear(32)
    OverlayFill(DLG_X, DLG_Y, DLG_W, DLG_H, 32, BODY)
    OverlayBox(DLG_X, DLG_Y, DLG_W, DLG_H, 2, BODY)
    dialog_text(DLG_X + (DLG_W - 6) // 2, DLG_Y, " APPS ", BODY)

    if #apps == 0 then
        dialog_text(DLG_X + 10, LIST_TOP + 4, "NO APPS INSTALLED", BODY)
    end
    local inner_x = DLG_X + 1
    local inner_w = DLG_W - 2
    for i = 1, PAGE do
        local app = apps[scroll + i]
        if not app then break end
        local row = LIST_TOP + (i - 1) * 2
        local attr = (scroll + i == selected) and HILITE or BODY
        OverlayFill(inner_x, row, inner_w, 2, 32, attr)
        if attr == HILITE then
            OverlayOut(inner_x, row, CH_POINTER, attr)
        end
        local name = app.name:upper():sub(1, inner_w - 10)
        dialog_text(inner_x + 2, row, name, attr)
        local tag = app.kind == "game" and "GAME" or (app.version ~= "" and ("V" .. app.version) or "")
        if tag ~= "" then
            tag = tag:sub(1, 7)
            dialog_text(inner_x + inner_w - #tag, row, tag, attr)
        end
        dialog_text(inner_x + 2, row + 1, app.description:sub(1, inner_w - 3), attr)
    end

    -- Scroll indicators on the frame's right edge.
    local edge = DLG_X + DLG_W - 1
    if scroll > 0 then
        OverlayOut(edge, LIST_TOP, CH_UP, BODY)
    end
    if scroll + PAGE < #apps then
        OverlayOut(edge, LIST_TOP + LIST_ROWS - 1, CH_DOWN, BODY)
    end

    local footer = string.char(CH_ARROW_UP, CH_ARROW_DOWN) ..
        " SELECT  RETURN RUN  ESC CLOSE"
    dialog_text(DLG_X + (DLG_W - #footer) // 2, DLG_Y + DLG_H - 1, footer, BODY)
end

open_apps_dialog = function()
    apps = load_apps()
    selected = 1
    scroll = 0
    dialog_open = true
    draw_dialog()
end

local function close_apps_dialog()
    dialog_open = false
    OverlayClear(32)
    paint()
end

local function dialog_key(key)
    if key == KEY_ESCAPE or key == KEY_RUNSTOP or key == 113 or key == 81 then
        close_apps_dialog()
        out("READY.")
    elseif key == KEY_UP then
        if selected > 1 then selected = selected - 1 end
    elseif key == KEY_DOWN then
        if selected < #apps then selected = selected + 1 end
    elseif key == KEY_HOME then
        selected = 1
    elseif key == KEY_RETURN then
        local app = apps[selected]
        close_apps_dialog()
        if app then
            run_program(app.path, {}, app.kind)
        else
            out("READY.")
        end
        return
    else
        return
    end
    if dialog_open then
        if selected <= scroll then
            scroll = selected - 1
        elseif selected > scroll + PAGE then
            scroll = selected - PAGE
        end
        draw_dialog()
    end
end

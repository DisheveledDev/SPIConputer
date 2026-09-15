-- Line editor: the command line is a plain string with a caret; each key
-- changes it and redraws the input row only. UP/DOWN walk the history.

local history = {}         -- recent command lines, oldest first
local history_pos = 0      -- 0 = editing a new line
local HISTORY_MAX = 30
local complete             -- TAB completion (assigned in 30-commands)

local function remember(line)
    if line == "" or history[#history] == line then return end
    history[#history + 1] = line
    if #history > HISTORY_MAX then table.remove(history, 1) end
end

local function set_input(text)
    input = text
    caret = #text
end

local function recall(step)
    local pos = history_pos + step
    if pos < 0 or pos > #history then return end
    history_pos = pos
    set_input(pos == 0 and "" or history[#history - pos + 1])
end

-- Returns true when the key changed the line (it is then redrawn).
local function edit_key(key)
    if key == KEY_BACKSPACE then
        if caret == 0 then return false end
        input = input:sub(1, caret - 1) .. input:sub(caret + 1)
        caret = caret - 1
    elseif key == KEY_DELETE then
        if caret >= #input then return false end
        input = input:sub(1, caret) .. input:sub(caret + 2)
    elseif key == KEY_LEFT then
        caret = math.max(caret - 1, 0)
    elseif key == KEY_RIGHT then
        caret = math.min(caret + 1, #input)
    elseif key == KEY_HOME then
        caret = 0
    elseif key == KEY_UP then
        recall(1)
    elseif key == KEY_DOWN then
        recall(-1)
    elseif key == KEY_ESCAPE then
        set_input("")
        history_pos = 0
    elseif key == KEY_TAB then
        complete()
    elseif key >= 32 and key < 127 then
        if #input >= 120 then return false end
        input = input:sub(1, caret) .. string.char(key) .. input:sub(caret + 1)
        caret = caret + 1
    else
        return false
    end
    return true
end

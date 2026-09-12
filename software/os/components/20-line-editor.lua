-- Line editor: the input line is a plain string with a caret index;
-- every key changes it and the shell repaints.

local function insert_char(ch)
    if #input >= COLS - 1 then
        return
    end
    input = input:sub(1, caret_col) .. ch .. input:sub(caret_col + 1)
    caret_col = caret_col + 1
end

local function backspace()
    if caret_col == 0 then
        return
    end
    input = input:sub(1, caret_col - 1) .. input:sub(caret_col + 1)
    caret_col = caret_col - 1
end

local function keyboard(event)
    local key = event.key
    if key == KEY_BACKSPACE then
        backspace()
    elseif key == KEY_DELETE then
        if caret_col < #input then
            input = input:sub(1, caret_col) .. input:sub(caret_col + 2)
        end
    elseif key == KEY_LEFT then
        caret_col = math.max(caret_col - 1, 0)
    elseif key == KEY_RIGHT then
        caret_col = math.min(caret_col + 1, #input)
    elseif key == KEY_HOME then
        caret_col = 0
    elseif key >= 32 and key < 127 then
        insert_char(string.char(key))
    end
end

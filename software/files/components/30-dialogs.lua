-- Dialogs on the overlay: a message, a Y/N question and a name prompt.
-- While one is open it takes every key.

local ASK_X1, ASK_Y1, ASK_X2, ASK_Y2 = 3, 11, 36, 17
local FIELD_W = ASK_X2 - ASK_X1 - 3

local function close_dialog()
    mode = nil
    on_answer = nil
    Overlay.Clear()
end

local function message(title, text)
    Overlay.Clear()
    Overlay.Dialog(title, { text, "", "press any key" }, DLG_ATTR)
    mode = "message"
end

local function confirm(text, fn)
    Overlay.Clear()
    Overlay.Dialog("CONFIRM", { text, "", "Y yes      N no" }, DLG_ATTR)
    mode = "confirm"
    on_answer = fn
end

-- The name field: the tail of the answer and a cursor block, one op.
local function draw_field()
    Overlay.Label(ASK_X1 + 2, ASK_Y1 + 3, FIELD_W, answer:sub(-(FIELD_W - 1)) .. "\219",
                  "left", Attributes.Normal)
end

local function ask(title, prompt, initial, fn)
    Overlay.Clear()
    Overlay.Window(ASK_X1, ASK_Y1, ASK_X2, ASK_Y2, title, Overlay.DOUBLE, DLG_ATTR)
    Overlay.OutText(ASK_X1 + 2, ASK_Y1 + 2, prompt, DLG_ATTR)
    Overlay.OutText(ASK_X1 + 2, ASK_Y2 - 1, "RETURN ok   ESC cancel", DLG_ATTR)
    answer = initial or ""
    draw_field()
    mode = "input"
    on_answer = fn
end

local function dialog_key(key)
    if mode == "message" then
        close_dialog()
    elseif mode == "confirm" then
        if key == 121 or key == 89 then           -- y
            local fn = on_answer
            close_dialog()
            fn()
        elseif key == 110 or key == 78 or key == Input.KEY_ESCAPE then
            close_dialog()
        end
    elseif mode == "input" then
        if key == Input.KEY_ESCAPE then
            close_dialog()
        elseif key == Input.KEY_RETURN then
            local fn, text = on_answer, answer
            close_dialog()
            if text ~= "" then fn(text) end
        elseif key == Input.KEY_BACKSPACE then
            answer = answer:sub(1, -2)
            draw_field()
        elseif key > 32 and key < 127 and #answer < 60 then
            answer = answer .. string.char(key)
            draw_field()
        end
    end
end

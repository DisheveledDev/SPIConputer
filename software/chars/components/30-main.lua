-- Entry points and keys.

function on_keypress(key)
    if key == Input.KEY_LEFT then
        move_to(code - 1)
    elseif key == Input.KEY_RIGHT then
        move_to(code + 1)
    elseif key == Input.KEY_UP then
        move_to(code - 16)
    elseif key == Input.KEY_DOWN or key == Input.KEY_RETURN or key == Input.KEY_SPACE then
        move_to(code + 16)
    elseif key == Input.KEY_HOME then
        move_to(0)
    elseif key == Input.KEY_ESCAPE or key == 113 or key == 81 then   -- q
        ExitProgram()
    end
end

function setup()
    Screen.Mode(1)
    draw_all()
end

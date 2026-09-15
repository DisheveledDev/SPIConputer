-- Entry points.

function setup()
    Screen.Mode(1)
    draw_all()
    Input.Keyboard.Callback("any", on_key)
    Input.Joystick.Callback(1, "any", on_joystick)
    Input.Joystick.Callback(2, "any", on_joystick)
end

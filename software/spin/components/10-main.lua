-- spin.lua — an example game.
--
-- A game takes the machine over: the shell that launched it is freed,
-- so all of the RAM and the screen belong to the game, and when it
-- exits the device restarts (the simulator boots again). This one just
-- spins a pointer until a key is pressed.

local frame = 0
local glyphs = { 16, 30, 17, 31 } -- right, up, left, down pointers

function setup()
    Screen.Mode(1)
    Screen.CenterText(12, "SPIN")
    Screen.CenterText(14, "a game owns the whole machine")
    Screen.CenterText(16, "press any key to quit and restart")
    Input.Keyboard.Callback("any", function()
        ExitProgram()
    end)
end

function tick()
    if WaitVSync() > 0 then
        frame = frame + 1
        Screen.Out(19, 20, glyphs[frame % 4 + 1])
    end
end

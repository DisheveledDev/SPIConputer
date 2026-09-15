-- Entry points.

function setup()
    Screen.Mode(1)
    math.randomseed(TimeNow())
    draw_walls()
    new_level()
end

function tick()
    -- Paced to the display: one step per frame, catching up after a
    -- long tick but never more than a few frames at once.
    local frames = WaitVSync()
    if frames <= 0 then return end
    read_input()
    for _ = 1, math.min(frames, 3) do
        step()
    end
end

-- Entry points.

function setup()
    Screen.Mode(1)
    math.randomseed(TimeNow())
    for w = 1, CELLS // 8 do           -- allocate the grid once
        grid[w] = 0
    end
    load_hiscore()
    draw_level()
    serve()
end

function tick()
    -- Paced to the display: steps are counted in frames, catching up
    -- after a long tick but never more than a few frames at once.
    local frames = WaitVSync()
    if frames <= 0 then return end
    read_input()
    if state ~= "play" then return end
    frame_acc = frame_acc + math.min(frames, 4)
    while frame_acc >= step_frames and state == "play" do
        frame_acc = frame_acc - step_frames
        step()
    end
end

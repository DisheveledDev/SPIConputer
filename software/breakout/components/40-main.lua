-- Entry points.

-- The music: popcorn.mod from the game's resources, streamed from the
-- card and looped; the effects play over it. M mutes and restores it.
local music = nil
music_on = true

function toggle_music()
    music_on = not music_on
    if not music then return end
    if music_on then music:Play() else music:Stop() end
end

function setup()
    Screen.Mode(1)
    math.randomseed(TimeNow())
    draw_walls()
    new_level()
    music = Music.LoadMod(app.resources .. "/popcorn.mod")
    if music then music:Play() end
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

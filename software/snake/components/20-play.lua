-- Play states: the serve / pause / game-over dialogs, levels and the
-- high score file.

local DIALOG_ATTR = Attributes.White + Attributes.Inverse

local function load_hiscore()
    local ok, text = pcall(fs.readall, HISCORE_FILE)
    hiscore = ok and tonumber(text and text:match("%d+")) or 0
end

local function save_hiscore()
    pcall(fs.writeall, HISCORE_FILE, tostring(hiscore) .. "\n")
end

local function dialog(title, lines)
    Overlay.Clear()
    Overlay.Dialog(title, lines, DIALOG_ATTR)
end

-- Waiting to start a level (the title screen is the level-1 serve).
local function serve()
    state = "serve"
    if level == 1 then
        dialog("SNAKE", {
            "Eat the hearts, grow, avoid walls",
            "and your own tail.",
            "",
            "Steer: cursor keys, WASD, joystick",
            "P pause    ESC quit",
            string.format("High score %d", hiscore),
            "",
            "SPACE or FIRE to start",
        })
    else
        dialog("LEVEL " .. level, {
            "New walls, and the snake is faster.",
            "",
            "SPACE or FIRE to go",
        })
    end
end

local function start()
    Overlay.Clear()
    state = "play"
    frame_acc = 0
end

local function pause()
    state = "paused"
    dialog("PAUSED", { "P to carry on", "ESC to quit" })
end

local function resume()
    Overlay.Clear()
    state = "play"
    frame_acc = 0
end

-- Crashed: mark the head, sound, save a new high score, show the result.
local function game_over()
    state = "over"
    Sound.Noise(300)
    draw_cell(head, 32, DEAD_ATTR)
    if new_hiscore then
        save_hiscore()
    end
    dialog("GAME OVER", {
        string.format("Score %d  (level %d, length %d)", score, level, length),
        new_hiscore and "NEW HIGH SCORE!" or string.format("High score %d", hiscore),
        "",
        "RETURN to play again, ESC to quit",
    })
end

local function next_level()
    level = level + 1
    Sound.Tone(660, 80)
    Sound.Tone(990, 120)
    draw_level()
    serve()
end

-- A new game from level 1, started straight away.
local function restart()
    score, level, new_hiscore = 0, 1, false
    draw_level()
    start()
end

local function add_score(points)
    score = score + points
    if score > hiscore then
        hiscore = score
        new_hiscore = true
    end
end

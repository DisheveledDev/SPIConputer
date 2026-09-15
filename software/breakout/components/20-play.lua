-- Play: serving, moving the ball and paddle, collisions, lives, levels.

-- The ball sits on the paddle until it is served.
local function place_on_paddle()
    ball_x = paddle_x + PADDLE_WIDTH / 2
    ball_y = PADDLE_ROW - 1 + 0.5
    ball_vx, ball_vy = 0, 0
end

local function start_serve()
    state = "serve"
    place_on_paddle()
    draw_ball()
    draw_message("BREAKOUT", {
        "SPACE or FIRE to serve",
        "cursor keys, Z/X or joystick to move",
    })
end

local function serve()
    Overlay.Clear()
    state = "play"
    ball_vx = (math.random() < 0.5 and -1 or 1) * speed * 0.7
    ball_vy = -speed
end

local function new_level()
    reset_bricks()
    speed = 0.28 + (level - 1) * 0.04
    paddle_x = (COLS - PADDLE_WIDTH) // 2
    draw_paddle()
    draw_hud()
    start_serve()
end

local function lose_ball()
    Sound.Noise(250)
    hide_ball()
    lives = lives - 1
    draw_hud()
    if lives <= 0 then
        state = "over"
        draw_message("GAME OVER", {
            "score " .. score,
            "",
            "RETURN to play again, ESC to quit",
        })
    else
        start_serve()
    end
end

-- A cell the ball cannot enter: a wall or a standing brick. Hitting a
-- brick removes it (and scores) as a side effect.
local function blocked(cx, cy)
    if cx < LEFT or cx > RIGHT or cy < TOP then
        Sound.Tone(220, 25, 120)
        return true
    end
    local row, col = brick_at(cx, cy)
    if row then
        remove_brick(row, col)
        Sound.Tone(660 + (#BRICK_ROWS - row) * 110, 30)
        return true
    end
    return false
end

-- One frame of ball movement: the x and y steps are taken separately so
-- a hit reflects the axis that caused it.
local function move_ball()
    local nx = ball_x + ball_vx
    if blocked(math.floor(nx), math.floor(ball_y)) then
        ball_vx = -ball_vx
    else
        ball_x = nx
    end

    local ny = ball_y + ball_vy
    local cx, cy = math.floor(ball_x), math.floor(ny)
    if cy == PADDLE_ROW and ball_vy > 0 then
        local px = math.floor(paddle_x)
        if cx >= px and cx < px + PADDLE_WIDTH then
            -- Reflect off the paddle; where it hits sets the angle.
            local offset = (ball_x - (px + PADDLE_WIDTH / 2)) / (PADDLE_WIDTH / 2)
            ball_vx = offset * speed * 1.2
            ball_vy = -speed
            if math.abs(ball_vx) < speed * 0.2 then
                ball_vx = (offset < 0 and -1 or 1) * speed * 0.2
            end
            Sound.Tone(440, 25)
            return
        end
    end
    if cy >= LOST_ROW then
        lose_ball()
        return
    end
    if blocked(cx, cy) then
        ball_vy = -ball_vy
    else
        ball_y = ny
    end
end

local function move_paddle()
    local dx = nudge
    nudge = 0
    if held_left then dx = dx - 0.5 end
    if held_right then dx = dx + 0.5 end
    if dx == 0 then return end
    paddle_x = math.max(LEFT, math.min(RIGHT - PADDLE_WIDTH + 1, paddle_x + dx))
    draw_paddle()
    if state == "serve" then
        place_on_paddle()
    end
end

-- One frame of play.
local function step()
    move_paddle()
    if state == "play" then
        move_ball()
        if state == "play" then
            draw_ball()
            if bricks_left == 0 then
                state = "clear"
                hide_ball()
                Sound.Tone(880, 200)
                draw_message("LEVEL " .. level .. " CLEARED", {
                    "score " .. score,
                    "",
                    "SPACE or FIRE for level " .. (level + 1),
                })
            end
        end
    elseif state == "serve" then
        draw_ball()
    end
end

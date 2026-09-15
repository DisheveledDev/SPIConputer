-- The board: walls, bricks, paddle, ball and the score line.

-- The score changes on every brick, so it is its own single write; the
-- rest of the line changes only with a life or a level.
local function draw_score()
    Screen.OutText(1, 0, string.format("SCORE %05d", score), Attributes.Yellow)
end

local function draw_hud()
    Screen.Clean(0, 0, COLS - 1, 0)
    draw_score()
    Screen.CenterText(0, "BREAKOUT", Attributes.Cyan)
    Screen.RightText(0, "LIVES " .. lives .. "  LV " .. level .. " ", Attributes.Green)
end

local function draw_walls()
    Screen.Fill(0, 1, COLS - 1, 1, 32, WALL_ATTR)
    Screen.Fill(0, 1, 0, ROWS - 1, 32, WALL_ATTR)
    Screen.Fill(COLS - 1, 1, COLS - 1, ROWS - 1, 32, WALL_ATTR)
end

local function brick_cells(row, col)
    local x = BRICK_LEFT + (col - 1) * BRICK_WIDTH
    local y = BRICK_TOP + row - 1
    return x, y, x + BRICK_WIDTH - 1
end

-- Every brick standing again, drawn a row at a time.
local function reset_bricks()
    bricks_left = 0
    for row = 1, #BRICK_ROWS do
        bricks[row] = {}
        for col = 1, BRICK_COLS do
            bricks[row][col] = true
            bricks_left = bricks_left + 1
        end
        local x1, y = brick_cells(row, 1)
        local _, _, x2 = brick_cells(row, BRICK_COLS)
        Screen.Fill(x1, y, x2, y, 32, BRICK_ROWS[row].colour + Attributes.Inverse)
    end
end

-- The brick standing at a cell, as (row, col), or nil.
local function brick_at(cx, cy)
    local row = cy - BRICK_TOP + 1
    if row < 1 or row > #BRICK_ROWS then return nil end
    local col = (cx - BRICK_LEFT) // BRICK_WIDTH + 1
    if col < 1 or col > BRICK_COLS then return nil end
    if bricks[row][col] then return row, col end
    return nil
end

local function remove_brick(row, col)
    bricks[row][col] = false
    bricks_left = bricks_left - 1
    local x1, y, x2 = brick_cells(row, col)
    Screen.Fill(x1, y, x2, y, 32, EMPTY_ATTR)
    score = score + BRICK_ROWS[row].points
    draw_score()
end

local function draw_paddle()
    local x = math.floor(paddle_x)
    if x == paddle_drawn then return end
    if paddle_drawn >= 0 then
        Screen.Fill(paddle_drawn, PADDLE_ROW, paddle_drawn + PADDLE_WIDTH - 1, PADDLE_ROW, 32, EMPTY_ATTR)
    end
    Screen.Fill(x, PADDLE_ROW, x + PADDLE_WIDTH - 1, PADDLE_ROW, 32, PADDLE_ATTR)
    paddle_drawn = x
end

local function draw_ball()
    local cx, cy = math.floor(ball_x), math.floor(ball_y)
    if cx == ball_cx and cy == ball_cy then return end
    if ball_cx >= 0 then
        Screen.Out(ball_cx, ball_cy, 32, EMPTY_ATTR)
    end
    Screen.Out(cx, cy, BALL_GLYPH, Attributes.White)
    ball_cx, ball_cy = cx, cy
end

local function hide_ball()
    if ball_cx >= 0 then
        Screen.Out(ball_cx, ball_cy, 32, EMPTY_ATTR)
    end
    ball_cx, ball_cy = -1, -1
end

local function draw_message(title, lines)
    Overlay.Clear()
    Overlay.Dialog(title, lines, Attributes.White + Attributes.Inverse)
end

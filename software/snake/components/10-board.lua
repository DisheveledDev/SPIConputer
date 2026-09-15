-- The board: walls, level layouts, cells, food, bonus and the score line.

-- Interior walls per level as {x1, y1, x2, y2} segments (inclusive).
-- Row 15 from column 6 to 12 stays clear in every layout: the snake
-- starts there heading right.
local LAYOUTS = {
    {},
    {
        { 10, 9, 29, 9 }, { 10, 21, 29, 21 },
    },
    {
        { 13, 5, 13, 12 }, { 13, 18, 13, 25 },
        { 26, 5, 26, 12 }, { 26, 18, 26, 25 },
    },
    {
        { 8, 8, 17, 8 }, { 22, 8, 31, 8 }, { 8, 22, 17, 22 }, { 22, 22, 31, 22 },
        { 8, 9, 8, 12 }, { 8, 18, 8, 21 }, { 31, 9, 31, 12 }, { 31, 18, 31, 21 },
    },
}

local function cell(x, y)
    return y * COLS + x + 1
end

-- The occupancy grid: cell c's four bits in integer (c - 1) // 8 + 1.
local function get(c)
    local i = c - 1
    return (grid[(i >> 3) + 1] >> ((i & 7) * 4)) & 15
end

local function set(c, v)
    local i = c - 1
    local w, shift = (i >> 3) + 1, (i & 7) * 4
    grid[w] = (grid[w] & ~(15 << shift)) | (v << shift)
end

local function cell_x(c)
    return (c - 1) % COLS
end

local function cell_y(c)
    return (c - 1) // COLS
end

-- One Screen.Out for a cell.
local function draw_cell(c, ch, attr)
    Screen.Out(cell_x(c), cell_y(c), ch, attr)
end

-- The whole score line in one write; fixed widths so nothing is left over.
-- It changes only on eating, so the length shown includes the segments
-- still to grow.
local function draw_hud()
    Screen.OutText(1, 0, string.format("SCORE %05d  HI %05d  LEN %3d  LV %2d",
                                       score, hiscore, length + grow, level), HUD_ATTR)
end

-- Mark and draw a wall rectangle.
local function wall(x1, y1, x2, y2)
    for y = y1, y2 do
        for x = x1, x2 do
            set(cell(x, y), WALL)
        end
    end
    Screen.Fill(x1, y1, x2, y2, 32, WALL_ATTR)
end

-- A random empty playfield cell (random probes, then a scan).
local function empty_cell()
    for _ = 1, 50 do
        local c = cell(math.random(LEFT, RIGHT), math.random(TOP, BOTTOM))
        if get(c) == EMPTY then return c end
    end
    local start = cell(math.random(LEFT, RIGHT), math.random(TOP, BOTTOM))
    for i = 0, CELLS - 1 do
        local c = (start - 1 + i) % CELLS + 1
        local x, y = cell_x(c), cell_y(c)
        if get(c) == EMPTY and x >= LEFT and x <= RIGHT and y >= TOP and y <= BOTTOM then
            return c
        end
    end
    return nil
end

local function place_food()
    local c = empty_cell()
    if c then
        set(c, FOOD)
        draw_cell(c, FOOD_CHAR, FOOD_ATTR)
    end
end

-- The bonus shows its remaining digit (9..1) on a purple block.
local function bonus_digit()
    return (bonus_steps + BONUS_DIGIT_STEPS - 1) // BONUS_DIGIT_STEPS
end

local function place_bonus()
    local c = empty_cell()
    if not c then return end
    bonus_cell, bonus_steps = c, 9 * BONUS_DIGIT_STEPS
    set(c, BONUS)
    draw_cell(c, 48 + bonus_digit(), BONUS_ATTR)
end

local function remove_bonus()
    if bonus_steps > 0 and get(bonus_cell) == BONUS then
        set(bonus_cell, EMPTY)
        draw_cell(bonus_cell, 32, EMPTY_ATTR)
    end
    bonus_cell, bonus_steps = 0, 0
end

-- One step of the bonus countdown: redraw only when the digit changes.
local function tick_bonus()
    if bonus_steps == 0 then return end
    local before = bonus_digit()
    bonus_steps = bonus_steps - 1
    if bonus_steps == 0 then
        bonus_steps = 1        -- let remove_bonus see it as present
        remove_bonus()
    elseif bonus_digit() ~= before then
        draw_cell(bonus_cell, 48 + bonus_digit(), BONUS_ATTR)
    end
end

local function update_speed()
    local frames = BASE_STEP_FRAMES - (level - 1) - length // SPEEDUP_LENGTH
    step_frames = math.max(MIN_STEP_FRAMES, frames)
end

-- The snake at its start: START_LENGTH cells on row 15, heading right
-- (every body cell points right, to the next segment).
local function place_snake()
    tail = cell(6, 15)
    head = cell(6 + START_LENGTH - 1, 15)
    for c = tail, head do
        set(c, BODY + 4 - 1)
    end
    length, grow = START_LENGTH, 0
    Screen.Fill(6, 15, 6 + START_LENGTH - 2, 15, 32, BODY_ATTR)
    draw_cell(head, 32, HEAD_ATTR)
    dir, queued = 4, 0
end

-- Draw a level from scratch: walls, layout, snake, food, score line.
local function draw_level()
    for w = 1, CELLS // 8 do
        grid[w] = 0
    end
    Screen.Clean(0, 0, COLS - 1, ROWS - 1)
    wall(0, 1, COLS - 1, 1)
    wall(0, ROWS - 1, COLS - 1, ROWS - 1)
    wall(0, 2, 0, ROWS - 2)
    wall(COLS - 1, 2, COLS - 1, ROWS - 2)
    for _, seg in ipairs(LAYOUTS[(level - 1) % #LAYOUTS + 1]) do
        wall(seg[1], seg[2], seg[3], seg[4])
    end
    place_snake()
    bonus_cell, bonus_steps = 0, 0
    eaten = 0
    frame_acc = 0
    place_food()
    update_speed()
    draw_hud()
end

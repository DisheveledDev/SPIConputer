-- breakout.lua — Breakout for the SPIComputer.
--
-- A game: it owns the machine (the shell is freed when it starts) and
-- the device restarts when it exits.
--
-- Everything is drawn in text mode 1 with attribute colours: the walls,
-- the bricks and the paddle are inverted spaces (Attributes.<colour> +
-- Attributes.Inverse paints a cell's background), so a brick row is one
-- Screen.Fill and knocking a brick out is one Screen.Fill back to
-- Attributes.Normal. The ball is a single glyph that moves in
-- fractional cell steps; only the cells that change are redrawn.
--
-- Layout (40x30):
--   row 0        score / title / lives
--   row 1        top wall; columns 0 and 39 the side walls
--   rows 3-8     six rows of 12 bricks, 3 cells wide, a colour per row
--   row 27       the paddle (7 cells)
--   row 29       the ball is lost below the paddle
--
-- Keys: cursor left/right (or Z / X, or joystick 1) move the paddle,
-- SPACE (or fire) serves the ball, RETURN restarts after game over,
-- ESC quits (the device restarts).
--
-- Built from the SPIEdit project in this folder: `swift run
-- --package-path ide/macos spibuild software/breakout`, or
-- software/install.sh to put it on the card image as games/breakout.game.

local COLS, ROWS = Screen.COLS, Screen.ROWS
local LEFT, RIGHT = 1, COLS - 2      -- playfield columns inside the walls
local TOP = 2                        -- first playfield row under the wall
local PADDLE_ROW = 27
local LOST_ROW = ROWS - 1            -- ball gone when it reaches this row
local PADDLE_WIDTH = 7
local BRICK_WIDTH = 3
local BRICK_COLS = 12
local BRICK_LEFT = 2                 -- 12 x 3 = 36 cells centred in 38
local BRICK_TOP = 3
local BALL_GLYPH = 254               -- ROM font: small square

local BRICK_ROWS = {                 -- colour and points per row, top first
    { colour = Attributes.Red,    points = 60 },
    { colour = Attributes.Orange, points = 50 },
    { colour = Attributes.Yellow, points = 40 },
    { colour = Attributes.Green,  points = 30 },
    { colour = Attributes.Cyan,   points = 20 },
    { colour = Attributes.Purple, points = 10 },
}

local WALL_ATTR = Attributes.Blue + Attributes.Inverse
local PADDLE_ATTR = Attributes.White + Attributes.Inverse
local EMPTY_ATTR = Attributes.Normal

-- Game state
local bricks = {}                    -- bricks[row][col] = true while standing
local bricks_left = 0
local score, lives, level = 0, 3, 1
local state = "serve"                -- serve | play | over | clear
local paddle_x = 0                   -- left cell (fractional while moving)
local paddle_drawn = -1              -- left cell as last drawn
local ball_x, ball_y = 0, 0          -- centre, in cells (fractional)
local ball_vx, ball_vy = 0, 0        -- cells per frame
local ball_cx, ball_cy = -1, -1      -- cell the ball is drawn in
local speed = 0.28
local held_left, held_right = false, false   -- keyboard or joystick
local key_left, key_right = false, false     -- keyboard alone (from events)
local nudge = 0                              -- whole cells queued by key taps
local fire_latched = false

-- snake.lua — Snake for the SPIComputer.
--
-- A game: it owns the machine (the shell is freed when it starts) and
-- the device restarts when it exits.
--
-- Drawn in text mode 1 with attribute colours on inverted spaces: the
-- walls are Attributes.Blue + Attributes.Inverse, the body Green, the
-- head Yellow. Food is a red heart glyph; now and then a bonus appears
-- as a purple digit that counts down and vanishes at zero (eat it early
-- for more points). Each step draws at most three cells: the new head,
-- the old head recoloured as body, and the tail cell it left behind.
-- The board is drawn in full only when a level starts.
--
-- One occupancy grid answers "what is in this cell" in one lookup: four
-- bits per cell, eight cells packed per integer (150 integers for the
-- whole screen, ~2 KB, where two 1200-entry tables would not fit the
-- 96 KB heap). A body cell stores the direction to the next segment
-- towards the head, so the tail follows the body by itself: the snake
-- is just its head and tail cell (y * 40 + x + 1), and a step allocates
-- nothing.
--
-- Layout (40x30):
--   row 0        score / high score / length / level
--   rows 1, 29   top and bottom walls; columns 0 and 39 the side walls
--   rows 2-28    the playfield (interior walls from level 2)
--
-- Every FOODS_PER_LEVEL foods the next level adds interior walls (four
-- layouts, then they repeat), speeds up and restarts the snake.
--
-- Keys: cursor keys or WASD (or joystick 1) steer, SPACE (or fire)
-- starts, P pauses, RETURN plays again after game over, ESC quits (the
-- device restarts). The high score is kept in /data/snake.hi.
--
-- Built from the SPIEdit project in this folder: `swift run
-- --package-path ide/macos spibuild software/snake`, or
-- software/install.sh to put it on the card image as games/snake.game.

local COLS, ROWS = Screen.COLS, Screen.ROWS
local LEFT, RIGHT = 1, COLS - 2      -- playfield columns inside the walls
local TOP, BOTTOM = 2, ROWS - 2      -- playfield rows inside the walls
local CELLS = COLS * ROWS

-- Tuning
local FOODS_PER_LEVEL = 10           -- foods eaten to finish a level
local GROW_PER_FOOD = 3              -- segments added per food
local START_LENGTH = 4
local BASE_STEP_FRAMES = 8           -- frames per step at level 1 (7.5 steps/s)
local MIN_STEP_FRAMES = 3            -- fastest: 20 steps/s
local SPEEDUP_LENGTH = 20            -- one frame faster per this many segments
local FOOD_POINTS = 10               -- times the level
local BONUS_CHANCE = 0.3             -- chance of a bonus after each food
local BONUS_DIGIT_STEPS = 6          -- steps each countdown digit lasts (9..1)
local HISCORE_FILE = "/data/snake.hi"

-- What a cell holds (the occupancy grid): BODY + dir - 1 is a body cell
-- whose next segment towards the head lies in direction dir.
local EMPTY, WALL, FOOD, BONUS, BODY = 0, 1, 2, 3, 4

local WALL_ATTR = Attributes.Blue + Attributes.Inverse
local BODY_ATTR = Attributes.Green + Attributes.Inverse
local HEAD_ATTR = Attributes.Yellow + Attributes.Inverse
local DEAD_ATTR = Attributes.Red + Attributes.Inverse
local BONUS_ATTR = Attributes.Purple + Attributes.Inverse
local FOOD_ATTR = Attributes.Red
local HUD_ATTR = Attributes.Cyan
local EMPTY_ATTR = Attributes.Normal
local FOOD_CHAR = 3                  -- ROM font: heart

-- Directions: 1 up, 2 down, 3 left, 4 right, and the cell-index step
local STEP = { -COLS, COLS, -1, 1 }
local OPPOSITE = { 2, 1, 4, 3 }

-- Game state
local grid = {}                      -- packed occupancy, 8 cells per integer
local head, tail = 0, 0              -- the snake's head and tail cells
local length = 0
local grow = 0                       -- segments still to add
local dir = 4
local queue = { 0, 0, 0 }            -- pending turns (quick turns are not lost)
local queued = 0
local score, hiscore, level = 0, 0, 1
local eaten = 0                      -- foods this level
local new_hiscore = false
local bonus_cell, bonus_steps = 0, 0 -- bonus position and steps left (0 = none)
local step_frames = BASE_STEP_FRAMES
local frame_acc = 0
local state = "serve"                -- serve | play | paused | over

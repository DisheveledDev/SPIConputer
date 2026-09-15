local W, H = 160, 120
local TOP, BOTTOM = 12, 0        -- the play area: below the HUD, above the banner
local frame = 0
local score = 0
local heading = 0
local over = false
local ship, rock
local stars = {}                 -- { x, y, colour }
local lo = true                  -- mode 11 (true) or mode 10

local SHIP = Graphics.Pixels({
    "....W....",
    "...WWW...",
    "...WCW...",
    "..WWCWW..",
    "..WWWWW..",
    ".WW.W.WW.",
    "WW..R..WW",
    "W...R...W",
}, { W = Graphics.LIGHTGREY, C = Graphics.CYAN, R = Graphics.RED })

local ROCK = Graphics.Pixels({
    "..GGGG..",
    ".GGDGGG.",
    "GGDDGGGG",
    "GGGGGDGG",
    "GGDGGGGG",
    "GGGGDDGG",
    ".GGGGGG.",
    "..GGGG..",
}, { G = Graphics.GREY, D = Graphics.DARKGREY })

-- Repaint the backdrop under a sprite: black, then the stars inside.
local function backdrop(x, y, w, h)
    Graphics.FillRect(x, y, w, h, Graphics.BLACK)
    for _, s in ipairs(stars) do
        if s[1] >= x and s[1] < x + w and s[2] >= y and s[2] < y + h then
            Graphics.PutPixel(s[1], s[2], s[3])
        end
    end
    return true
end

local function hud()
    Graphics.Text(2, 2, string.format("SCORE %04d  HDG %3d", score, heading),
                  Graphics.WHITE, Graphics.BLACK)
end

local function start()
    Graphics.Mode(lo and 11 or 10)
    W, H = Graphics.WIDTH, Graphics.HEIGHT
    Graphics.Clear(Graphics.BLACK)
    stars = {}
    BOTTOM = H - 34
    for i = 1, 40 do
        stars[i] = { math.random(0, W - 1), math.random(TOP, BOTTOM - 1), Graphics.Grey(math.random(60, 220)) }
        Graphics.PutPixel(stars[i][1], stars[i][2], stars[i][3])
    end
    ship = Graphics.Sprite(9, 8, SHIP, 0)
    ship.background = backdrop
    rock = Graphics.Sprite(8, 8, ROCK, 0)
    rock.background = backdrop
    rock.vx, rock.vy = 1, 1
    ship:Draw(W // 2 - 4, (TOP + BOTTOM) // 2)
    rock:Draw(10, 20)
    score, heading, over = 0, 0, false
    hud()
    Graphics.CenterText(H - 30, "PIXELS", Graphics.YELLOW, nil, 3)
    Graphics.CenterText(H - 8, "LEFT/RIGHT TURN  UP/DOWN MOVE  SPACE MODE", Graphics.LIGHTBLUE)
end

local function step()
    frame = frame + 1
    -- The starfield drifts left: the sprites come off first (so they do
    -- not smear), one Scroll moves everything, the stars that wrapped
    -- are replaced, and the sprites go back on. Four ops plus the wraps.
    if frame % 2 == 0 then
        ship:Hide()
        rock:Hide()
        Graphics.ScrollRect(0, TOP, W, BOTTOM - TOP, -1, 0, Graphics.BLACK)
        for _, s in ipairs(stars) do
            s[1] = s[1] - 1
            if s[1] < 0 then
                s[1], s[2] = W - 1, math.random(TOP, BOTTOM - 1)
                Graphics.PutPixel(s[1], s[2], s[3])
            end
        end
        ship:Draw()
        rock:Draw()
    end
    if over then return end
    -- The asteroid bounces around the play area.
    local nx, ny = rock.x + rock.vx, rock.y + rock.vy
    if nx < 0 or nx + rock.w > W then rock.vx = -rock.vx nx = rock.x + rock.vx end
    if ny < TOP or ny + rock.h > BOTTOM then rock.vy = -rock.vy ny = rock.y + rock.vy end
    rock:MoveTo(nx, ny)
    if frame % 30 == 0 then
        score = score + 1
        hud()
    end
    if Graphics.Overlaps(ship, rock) then
        over = true
        Graphics.CenterText(H // 2 - 8, "HIT!", Graphics.RED, Graphics.BLACK, 2)
    end
end

function on_keypress(key)
    if key == Input.KEY_ESCAPE then ExitProgram() end
    if key == Input.KEY_SPACE then lo = not lo start() return end
    if over then return end
    if key == Input.KEY_LEFT then
        heading = (heading - 15) % 360
        ship:Turn(heading)
    elseif key == Input.KEY_RIGHT then
        heading = (heading + 15) % 360
        ship:Turn(heading)
    elseif key == Input.KEY_UP then
        ship:MoveBy(0, -2)
    elseif key == Input.KEY_DOWN then
        ship:MoveBy(0, 2)
    end
    hud()
end

function setup()
    math.randomseed(TimeNow())
    start()
end

function tick()
    local frames = WaitVSync()
    if frames <= 0 then return end
    for _ = 1, math.min(frames, 3) do step() end
end

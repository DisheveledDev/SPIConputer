-- SDK: Graphics
-- Summary: Pixels, shapes, sprites (with rotation, flips and scaling), scrolling and text on the pixel modes 10 (320x240) and 11 (160x120).
-- Namespaces: Graphics
--
-- Same format contract as screen.lua (preamble, `function Name.Sub`
-- blocks stripped when unused, `---` signature lines). Methods on sprite
-- objects (`s:MoveTo(x, y)`) dispatch to `Graphics.MoveTo(s, x, y)`.
--
-- The pixel modes are one palette byte per pixel: mode 10 is 320x240
-- (each pixel shown 2x2), mode 11 is 160x120 (each pixel 4x4, a 19 KB
-- buffer and the cheapest to draw and display). Coordinates are pixels
-- from the top-left; colours are palette indexes 0-255. The default
-- palette: 0-15 the text colours (named below), 16-231 a 6x6x6 colour
-- cube (Graphics.RGB), 232-255 a grey ramp (Graphics.Grey);
-- Graphics.Palette changes any entry, which recolours everything drawn
-- in it (palette animation).
--
-- Every drawing call is one display op, done by the display core at the
-- next frame and clipped there, so shapes and sprites may run off the
-- edges. Draw each frame's changes only: a sprite's MoveTo is two ops
-- (erase, draw), a Scroll one. Images are strings of w*h palette bytes,
-- row by row: Pixels builds one from ASCII art; Rotate, Flip and Scale
-- make new ones (in Lua, so do it once and keep the frames, not every
-- tick: a rotated w x h image is up to (w+h)^2 bytes).

Graphics = Graphics or {}
Graphics.WIDTH = 320       -- pixels across in the current mode (Mode sets)
Graphics.HEIGHT = 240      -- pixels down: 240 in mode 10, 120 in mode 11

-- The first 16 palette entries (the text colours), by name.
Graphics.BLACK = 0
Graphics.WHITE = 1
Graphics.RED = 2
Graphics.CYAN = 3
Graphics.PURPLE = 4
Graphics.GREEN = 5
Graphics.BLUE = 6
Graphics.YELLOW = 7
Graphics.ORANGE = 8
Graphics.BROWN = 9
Graphics.PINK = 10
Graphics.DARKGREY = 11
Graphics.GREY = 12
Graphics.LIGHTGREEN = 13
Graphics.LIGHTBLUE = 14
Graphics.LIGHTGREY = 15

-- Sprite objects: { w, h, pixels, key, x, y, shown, background, frames }.
local sprite_mt = {
    __index = function(_, key)
        return Graphics[key]
    end,
}

-- Nearest-neighbour rotation of a w x h image about its centre into a
-- side x side image, unset pixels `key`. Angles are degrees clockwise.
local function rotate_any(pixels, w, h, angle, key)
    local side = math.ceil(math.sqrt(w * w + h * h))
    local rad = math.rad(angle)
    local c, s = math.cos(rad), math.sin(rad)
    local cx, cy = (w - 1) / 2, (h - 1) / 2
    local ox, oy = (side - 1) / 2, (side - 1) / 2
    local keych = string.char(key)
    local out = {}
    for y = 0, side - 1 do
        local dy = y - oy
        local row = {}
        for x = 0, side - 1 do
            local dx = x - ox
            -- Inverse map: where in the source does this output pixel come from?
            local sx = math.floor(cx + dx * c + dy * s + 0.5)
            local sy = math.floor(cy - dx * s + dy * c + 0.5)
            if sx >= 0 and sx < w and sy >= 0 and sy < h then
                local i = sy * w + sx + 1
                row[x + 1] = pixels:sub(i, i)
            else
                row[x + 1] = keych
            end
        end
        out[y + 1] = table.concat(row)
    end
    return table.concat(out), side, side
end

-- The four right-angle rotations are exact and keep every pixel.
local function rotate_right(pixels, w, h, quarter)
    if quarter == 0 then return pixels, w, h end
    local out = {}
    if quarter == 2 then
        for i = #pixels, 1, -1 do out[#out + 1] = pixels:sub(i, i) end
        return table.concat(out), w, h
    end
    -- 90 clockwise: output (x, y) = source (y, h-1-x); 270: (w-1-y, x).
    for y = 0, w - 1 do
        for x = 0, h - 1 do
            local sx, sy
            if quarter == 1 then sx, sy = y, h - 1 - x else sx, sy = w - 1 - y, x end
            local i = sy * w + sx + 1
            out[#out + 1] = pixels:sub(i, i)
        end
    end
    return table.concat(out), h, w
end

--- Graphics.Mode([mode])
-- Selects pixel mode 10 (320x240, the default) or 11 (160x120),
-- clearing the screen to colour 0, and sets Graphics.WIDTH/HEIGHT.
-- Pixel modes have no text cells: Graphics.Text draws text as pixels.
function Graphics.Mode(mode)
    mode = mode or 10
    local ok, err = ScreenMode(mode)
    if ok then
        Graphics.WIDTH = mode == 11 and 160 or 320
        Graphics.HEIGHT = mode == 11 and 120 or 240
    end
    return ok, err
end

--- Graphics.PutPixel(x, y, colour)
-- Sets one pixel. One op per pixel: for shapes use Line, Rect, Circle
-- and Blit, which are one op each.
function Graphics.PutPixel(x, y, colour)
    return ScreenPlot(x, y, colour)
end

--- Graphics.Clear([colour])
-- Fills the whole screen with `colour` (default 0, black). One op.
function Graphics.Clear(colour)
    return ScreenClear(colour or 0)
end

--- Graphics.Line(x1, y1, x2, y2, colour)
-- A straight line between two points, both included.
function Graphics.Line(x1, y1, x2, y2, colour)
    return ScreenPixelLine(x1, y1, x2, y2, colour)
end

--- Graphics.Rect(x, y, w, h, colour)
-- A rectangle outline, w by h pixels from (x, y).
function Graphics.Rect(x, y, w, h, colour)
    return ScreenPixelRect(x, y, w, h, colour, false)
end

--- Graphics.FillRect(x, y, w, h, colour)
-- A filled rectangle, w by h pixels from (x, y).
function Graphics.FillRect(x, y, w, h, colour)
    return ScreenPixelRect(x, y, w, h, colour, true)
end

--- Graphics.Circle(cx, cy, r, colour)
-- A circle outline of radius r around (cx, cy).
function Graphics.Circle(cx, cy, r, colour)
    return ScreenPixelCircle(cx, cy, r, colour, false)
end

--- Graphics.FillCircle(cx, cy, r, colour)
-- A filled circle of radius r around (cx, cy).
function Graphics.FillCircle(cx, cy, r, colour)
    return ScreenPixelCircle(cx, cy, r, colour, true)
end

--- Graphics.Text(x, y, text, colour [, bg [, scale]])
-- Text as pixels with the 8x8 font (or the program's own tiles), from
-- pixel (x, y): 8 pixels a character, or 8 * scale with `scale` 2-4.
-- With `bg` each character's cell is painted first; without it only the
-- letters are drawn over what is there. One op.
function Graphics.Text(x, y, text, colour, bg, scale)
    return ScreenPixelText(x, y, tostring(text), colour, bg, scale or 1)
end

--- Graphics.TextWidth(text [, scale])
-- The width in pixels Graphics.Text gives `text`: 8 * scale a character.
function Graphics.TextWidth(text, scale)
    return #tostring(text) * 8 * (scale or 1)
end

--- Graphics.CenterText(y, text, colour [, bg [, scale]])
-- Graphics.Text centred across the screen on pixel row y.
function Graphics.CenterText(y, text, colour, bg, scale)
    text = tostring(text)
    local x = (Graphics.WIDTH - #text * 8 * (scale or 1)) // 2
    return ScreenPixelText(x, y, text, colour, bg, scale or 1)
end

--- Graphics.Blit(x, y, w, h, pixels [, key])
-- Copies a w by h image, `pixels` a string of w*h palette bytes row by
-- row, to (x, y). With `key`, pixels of that colour are transparent. An
-- image over 8184 pixels is sent in bands of rows (one op each).
function Graphics.Blit(x, y, w, h, pixels, key)
    local rows = math.max(1, 8184 // w)
    if rows >= h then
        return ScreenBlit(x, y, w, h, pixels, key)
    end
    local ok, err = true, nil
    for top = 0, h - 1, rows do
        local n = math.min(rows, h - top)
        ok, err = ScreenBlit(x, y + top, w, n, pixels:sub(top * w + 1, (top + n) * w), key)
        if not ok then return ok, err end
    end
    return ok, err
end

--- Graphics.Scroll(dx, dy [, fill])
-- Shifts the whole screen by (dx, dy) pixels (-127..127); the pixels
-- uncovered get `fill` (default 0). One op: a scrolling backdrop costs
-- one Scroll plus drawing the new edge.
function Graphics.Scroll(dx, dy, fill)
    return ScreenPixelScroll(0, 0, Graphics.WIDTH, Graphics.HEIGHT, dx, dy, fill or 0)
end

--- Graphics.ScrollRect(x, y, w, h, dx, dy [, fill])
-- Shifts the pixels of one rectangle by (dx, dy); pixels uncovered inside
-- it get `fill` (default 0).
function Graphics.ScrollRect(x, y, w, h, dx, dy, fill)
    return ScreenPixelScroll(x, y, w, h, dx, dy, fill or 0)
end

--- Graphics.RGB(r, g, b)
-- The palette index nearest a colour (each 0-255) in the default
-- palette's 6x6x6 cube (entries 16-231).
function Graphics.RGB(r, g, b)
    local function level(v)
        v = math.max(0, math.min(255, v or 0))
        return (v + 25) // 51
    end
    return 16 + 36 * level(r) + 6 * level(g) + level(b)
end

--- Graphics.Grey(level)
-- The palette index nearest a grey (0 black .. 255 white): the default
-- palette's grey ramp (entries 232-255), or black or white.
function Graphics.Grey(level)
    level = math.max(0, math.min(255, level or 0))
    if level < 4 then return Graphics.BLACK end
    if level > 246 then return Graphics.WHITE end
    return 232 + math.max(0, math.min(23, (level - 3) // 10))
end

--- Graphics.Palette(index, r, g, b)
-- Sets palette entry `index` (0-255) to a colour; everything drawn in
-- that index changes at once (palette animation). Entries 0-8 are also
-- the text colours.
function Graphics.Palette(index, r, g, b)
    return ScreenPalette(index, r, g, b)
end

--- Graphics.Pixels(rows, map [, key])
-- Builds an image from ASCII art: `rows` a list of equal-length strings,
-- `map` a table from character to colour; characters not in the map
-- become `key` (default 0). Returns pixels, w, h for Blit or Sprite.
function Graphics.Pixels(rows, map, key)
    local w, parts = #rows[1], {}
    for i, row in ipairs(rows) do
        local out = {}
        for c = 1, w do
            out[c] = string.char(map[row:sub(c, c)] or key or 0)
        end
        parts[i] = table.concat(out)
    end
    return table.concat(parts), w, #rows
end

--- Graphics.Rotate(pixels, w, h, angle [, key])
-- A copy of a w x h image turned `angle` degrees clockwise. 90, 180 and
-- 270 keep every pixel (the result is h x w for 90 and 270); any other
-- angle gives a square image big enough for the turned shape, the gaps
-- filled with `key` (default 0). Returns pixels, w, h.
function Graphics.Rotate(pixels, w, h, angle, key)
    angle = angle % 360
    if angle % 90 == 0 then
        return rotate_right(pixels, w, h, angle // 90)
    end
    return rotate_any(pixels, w, h, angle, key or 0)
end

--- Graphics.Flip(pixels, w, h [, horizontal [, vertical]])
-- A mirrored copy of an image: left-right when `horizontal` is true,
-- top-bottom when `vertical` is (both by default). Returns pixels.
function Graphics.Flip(pixels, w, h, horizontal, vertical)
    if horizontal == nil and vertical == nil then horizontal, vertical = true, true end
    local out = {}
    for y = 0, h - 1 do
        local sy = vertical and (h - 1 - y) or y
        local row = pixels:sub(sy * w + 1, sy * w + w)
        out[y + 1] = horizontal and row:reverse() or row
    end
    return table.concat(out)
end

--- Graphics.Scale(pixels, w, h, factor)
-- An image enlarged by a whole `factor` (2 doubles every pixel).
-- Returns pixels, w * factor, h * factor.
function Graphics.Scale(pixels, w, h, factor)
    factor = math.max(1, math.floor(factor or 2))
    local out = {}
    for y = 0, h - 1 do
        local row = {}
        for x = 1, w do
            local i = y * w + x
            row[x] = string.rep(pixels:sub(i, i), factor)
        end
        local line = table.concat(row)
        for _ = 1, factor do out[#out + 1] = line end
    end
    return table.concat(out), w * factor, h * factor
end

--- Graphics.Sprite(w, h, pixels [, key])
-- A sprite object for a w x h image (`pixels` as for Blit). `key` is its
-- transparent colour. Show it with s:Draw(x, y), move it with
-- s:MoveTo(x, y) (which repaints what it uncovers with s.background: a
-- colour, default 0, or a function(x, y, w, h) that redraws that area),
-- change its picture with s:SetImage or s:Turn. Fields: x, y, w, h.
function Graphics.Sprite(w, h, pixels, key)
    return setmetatable({
        w = w, h = h, pixels = pixels, key = key,
        x = 0, y = 0, shown = false, background = 0,
        base = { pixels = pixels, w = w, h = h }, turned = {},
    }, sprite_mt)
end

--- Graphics.Draw(sprite [, x, y])
-- Draws the sprite at (x, y) (default: where it is), without erasing
-- anything: use it to show a sprite, or to redraw one after the screen
-- was cleared or scrolled.
function Graphics.Draw(sprite, x, y)
    sprite.x, sprite.y = x or sprite.x, y or sprite.y
    sprite.shown = true
    return ScreenBlit(sprite.x, sprite.y, sprite.w, sprite.h, sprite.pixels, sprite.key)
end

--- Graphics.Hide(sprite)
-- Removes the sprite: its area is repainted with its background.
function Graphics.Hide(sprite)
    if not sprite.shown then return true end
    sprite.shown = false
    local bg = sprite.background
    if type(bg) == "function" then
        return bg(sprite.x, sprite.y, sprite.w, sprite.h)
    end
    return ScreenPixelRect(sprite.x, sprite.y, sprite.w, sprite.h, bg or 0, true)
end

--- Graphics.MoveTo(sprite, x, y)
-- Moves the sprite: the old area is repainted with its background, then
-- the sprite is drawn at (x, y). Two ops; nothing happens when it has
-- not moved.
function Graphics.MoveTo(sprite, x, y)
    if sprite.shown and sprite.x == x and sprite.y == y then return true end
    Graphics.Hide(sprite)
    return Graphics.Draw(sprite, x, y)
end

--- Graphics.MoveBy(sprite, dx, dy)
-- Moves the sprite by (dx, dy) pixels: MoveTo relative to where it is.
function Graphics.MoveBy(sprite, dx, dy)
    return Graphics.MoveTo(sprite, sprite.x + dx, sprite.y + dy)
end

--- Graphics.SetImage(sprite, pixels, w, h)
-- Gives the sprite a new picture (an animation frame); a shown sprite is
-- redrawn in place, erased first if the size changed. The picture also
-- becomes the one Turn rotates.
function Graphics.SetImage(sprite, pixels, w, h)
    local resized = w ~= sprite.w or h ~= sprite.h
    if sprite.shown and resized then Graphics.Hide(sprite) end
    sprite.pixels, sprite.w, sprite.h = pixels, w, h
    sprite.base = { pixels = pixels, w = w, h = h }
    sprite.turned = {}
    if sprite.shown or resized then return Graphics.Draw(sprite) end
    return true
end

--- Graphics.Turn(sprite, angle)
-- Shows the sprite's picture rotated `angle` degrees clockwise (from
-- its unrotated image, so angles do not add up), keeping the rotated
-- images so a repeated angle costs nothing: round angles to a step (say
-- 15 degrees) to bound the memory. A shown sprite is redrawn, centred
-- where it was.
function Graphics.Turn(sprite, angle)
    angle = math.floor(angle) % 360
    local frame = sprite.turned[angle]
    if not frame then
        local pixels, w, h = Graphics.Rotate(sprite.base.pixels, sprite.base.w,
                                            sprite.base.h, angle, sprite.key or 0)
        frame = { pixels = pixels, w = w, h = h }
        sprite.turned[angle] = frame
    end
    if frame.pixels == sprite.pixels then return true end
    local cx, cy = sprite.x + sprite.w // 2, sprite.y + sprite.h // 2
    local shown = sprite.shown
    if shown then Graphics.Hide(sprite) end
    sprite.pixels, sprite.w, sprite.h = frame.pixels, frame.w, frame.h
    sprite.x, sprite.y = cx - frame.w // 2, cy - frame.h // 2
    if shown then return Graphics.Draw(sprite) end
    return true
end

--- Graphics.Overlaps(a, b)
-- True when two sprites' rectangles overlap (a simple hit test).
function Graphics.Overlaps(a, b)
    return a.x < b.x + b.w and b.x < a.x + a.w and a.y < b.y + b.h and b.y < a.y + a.h
end

--- Graphics.OnScreen(sprite)
-- True while any part of the sprite is inside the screen.
function Graphics.OnScreen(sprite)
    return sprite.x + sprite.w > 0 and sprite.x < Graphics.WIDTH and
        sprite.y + sprite.h > 0 and sprite.y < Graphics.HEIGHT
end

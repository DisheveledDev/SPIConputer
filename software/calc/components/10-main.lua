-- The sandbox: math functions by their short names and nothing else.

local USAGE = "USAGE: CALC EXPRESSION (CALC 2+3*4)"

local function hex(n)
    n = math.tointeger(n)
    if not n then error("hex needs a whole number", 0) end
    return string.format("0x%X", n)
end

local function sandbox()
    return {
        sqrt = math.sqrt, sin = math.sin, cos = math.cos, tan = math.tan,
        asin = math.asin, acos = math.acos, atan = math.atan,
        abs = math.abs, floor = math.floor, ceil = math.ceil,
        min = math.min, max = math.max, log = math.log, exp = math.exp,
        fmod = math.fmod, random = math.random,
        pi = math.pi, huge = math.huge, hex = hex,
    }
end

-- A value as calc prints it: integers plainly (plus hex), floats to ten
-- significant digits, anything else as text.
local function show(v)
    if math.type(v) == "integer" then
        return tostring(v), string.format("0x%X", v)
    elseif math.type(v) == "float" then
        if v == math.floor(v) and math.abs(v) < 1e15 then
            return string.format("%.1f", v)
        end
        return string.format("%.10g", v)
    end
    return tostring(v)
end

function setup()
    if #args == 0 then
        UtilityResult(false, USAGE)
        return
    end
    -- The shell turns words that look like file names into card paths;
    -- a leading /data/ cannot be part of an expression, so drop it.
    local words = {}
    for i = 1, #args do
        local word = args[i]:gsub("^/data/", "")
        words[#words + 1] = word
    end
    local expr = table.concat(words, " ")
    if expr:find("%f[%w_]function%f[^%w_]") then
        UtilityResult(false, "FUNCTIONS ARE NOT ALLOWED")
        return
    end
    local chunk, err = load("return " .. expr, "=calc", "t", sandbox())
    if not chunk then
        UtilityResult(false, "SYNTAX: " .. tostring(err):gsub("^calc:1: ", ""))
        return
    end
    local results = table.pack(pcall(chunk))
    if not results[1] then
        local message = tostring(results[2]):gsub("^calc:1: ", "")
        -- A name outside the sandbox reads better than Lua's wording.
        local name = message:match("global '([%w_]+)'")
        if name then message = "UNKNOWN NAME: " .. name end
        UtilityResult(false, message)
        return
    end
    if results.n < 2 then
        UtilityResult(false, "NO VALUE")
        return
    end
    local shown, hexes = {}, {}
    for i = 2, results.n do
        local text, h = show(results[i])
        shown[#shown + 1] = text
        if h then hexes[#hexes + 1] = h end
    end
    local line = "= " .. table.concat(shown, ", ")
    if #hexes == #shown and #shown > 0 then
        line = line .. "   (" .. table.concat(hexes, ", ") .. ")"
    end
    UtilityResult(true, line)
end

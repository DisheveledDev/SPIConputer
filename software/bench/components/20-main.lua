local COLS = 40
local results = {}        -- { name, ms, ops_per_ms }
local next_test = 1
local finished = false

local function text(x, row, message, attr)
    for i = 1, math.min(#message, COLS - x) do
        ScreenOut(x + i - 1, row, message:byte(i), attr or 0)
    end
end

-- Describe the Lua build in one line: the integer width and float
-- precision tell you whether LUA_32BITS is in effect.
local function build_line()
    local bits = math.maxinteger > 2147483647 and 64 or 32
    local fbits = (2 ^ 24 + 1 == 2 ^ 24) and 32 or 64
    return string.format("%s int%d float%d", _VERSION, bits, fbits)
end

local function result_line(r)
    return string.format("%-14s %6dms %8d/ms", r.name, r.ms, r.ops_per_ms)
end

local function draw()
    ScreenClear(32)
    text(0, 0, "SPICOMPUTER LUA BENCHMARK", 0x80)
    text(0, 1, build_line(), 0)
    text(0, 3, string.format("%-14s %8s %11s", "test", "time", "rate"), 0x02)
    for i, r in ipairs(results) do
        text(0, 3 + i, result_line(r), 0)
    end
    if finished then
        text(0, 5 + #results, string.format("gc full stall  %6dms", gc_stall_ms or 0), 0)
        text(0, 6 + #results, "saved to /data/bench.txt", 0x02)
        text(0, 7 + #results, "! = out of heap (see file)", 0x02)
        text(0, 9 + #results, "press any key to exit", 0x80)
    elseif next_test <= #tests then
        text(0, 4 + #results, "running " .. tests[next_test].name .. "...", 0x02)
    end
end

local function save()
    local lines = { build_line(), "test,ms,ops_per_ms" }
    for _, r in ipairs(results) do
        lines[#lines + 1] = string.format("%s,%d,%d%s", r.name, r.ms,
                                          r.ops_per_ms,
                                          r.err and ("," .. r.err) or "")
    end
    lines[#lines + 1] = string.format("gc full stall,%d,0", gc_stall_ms or 0)
    fs.writeall("/data/bench.txt", table.concat(lines, "\n") .. "\n")
end

function setup()
    ScreenMode(1)
    draw()
end

-- One test per tick: the scheduler feeds the watchdog between ticks,
-- and each test is sized to stay well under its 2 s period.
function tick()
    if finished then
        return
    end
    local t = tests[next_test]
    if not t then
        finished = true
        save()
        draw()
        return
    end
    collectgarbage("collect") -- start each test with a clean heap
    local t0 = TimeNow()
    local ok, err = pcall(t.fn)
    local ms = TimeNow() - t0
    -- A test that runs out of heap is a result too (the cap, not the
    -- VM, is what it measured): record it as a zero rate.
    results[#results + 1] = {
        name = ok and t.name or (t.name .. "!"),
        ms = ms,
        ops_per_ms = (ok and ms > 0) and math.floor(t.ops / ms)
                     or (ok and t.ops or 0),
        err = not ok and tostring(err) or nil,
    }
    next_test = next_test + 1
    draw()
end

function on_keypress()
    if finished then
        ExitProgram()
    end
end

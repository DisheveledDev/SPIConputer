-- Each test is { name, ops, fn }: fn runs a fixed amount of work and
-- returns a checksum (kept so the interpreter cannot drop the loop).
-- `ops` is the number of operations the test counts as, for the
-- ops/ms figure. Names fit the 14-column label on screen.
--
-- Sizes are chosen to take 100-500 ms each on the board at 252 MHz;
-- the whole set stays well inside the 2 s watchdog per tick.

local N_INT = 200000
local N_FLOAT = 200000
local N_CALL = 200000
-- Live-set sizes are held well under the 64 KB heap cap: an array grows
-- by doubling (old and new blocks coexist during the realloc) and the
-- previous rep's table is still garbage while the next one fills.
local N_ARRAY = 1000       -- 1000 numbers = 16 KB of TValues at 16 B
local N_ARRAY_REPS = 40
local N_HASH = 250
local N_HASH_REPS = 40
local N_STRING = 3000
local N_API = 50000
local N_API_TABLE = 20000
-- Kept under the 1024-op display queue (with the screen redraw's own
-- ops): a full queue blocks until core 0's next frame drain on the
-- board and, because the simulator is single-threaded, hangs it.
local N_SCREEN = 600

tests = {}

local function add(name, ops, fn)
    tests[#tests + 1] = { name = name, ops = ops, fn = fn }
end

-- Integer arithmetic in a tight loop (the common case in game code).
add("int loop", N_INT, function()
    local s = 0
    for i = 1, N_INT do
        s = s + (i * 3) % 7
    end
    return s
end)

-- Float arithmetic: double soft-float today, single after LUA_32BITS.
add("float loop", N_FLOAT, function()
    local x = 1.5
    for i = 1, N_FLOAT do
        x = x * 1.000001 + 0.25
        if x > 1e6 then x = x - 1e6 end
    end
    return math.floor(x)
end)

-- Lua-to-Lua calls: frame overhead of the VM.
add("lua call", N_CALL, function()
    local function f(a, b)
        return a + b
    end
    local s = 0
    for i = 1, N_CALL do
        s = f(s, i)
    end
    return s
end)

-- Array part: fill and read back; each rep allocates a fresh table so
-- the collector has to work (this is what a 64 KB cap makes expensive).
add("table array", N_ARRAY * N_ARRAY_REPS * 2, function()
    local s = 0
    for _ = 1, N_ARRAY_REPS do
        local t = {}
        for i = 1, N_ARRAY do
            t[i] = i
        end
        for i = 1, N_ARRAY do
            s = s + t[i]
        end
    end
    return s
end)

-- Hash part with string keys: interning plus hashing plus lookup.
add("table hash", N_HASH * N_HASH_REPS * 2, function()
    local s = 0
    for _ = 1, N_HASH_REPS do
        local t = {}
        for i = 1, N_HASH do
            t["k" .. i] = i
        end
        for i = 1, N_HASH do
            s = s + t["k" .. i]
        end
    end
    return s
end)

-- String building and scanning, the text-heavy path (shell, editor).
add("string ops", N_STRING, function()
    local s = 0
    local base = string.rep("abcdefgh", 25) -- 200 chars
    for i = 1, N_STRING do
        local line = base:sub(1 + (i % 100), 40 + (i % 100)) .. i
        s = s + line:byte(1) + #line
    end
    return s
end)

-- C API call with no result table: the OS call overhead itself
-- (argument checks plus finding the program from the Lua state).
add("api TimeNow", N_API, function()
    local s = 0
    for _ = 1, N_API do
        s = s + TimeNow()
    end
    return s
end)

-- C API call that returns a table per call: allocation on the hot path.
add("api InputCtl", N_API_TABLE, function()
    local s = 0
    for _ = 1, N_API_TABLE do
        local c = InputControl(1)
        if c.fire then s = s + 1 end
    end
    return s
end)

-- The per-cell display call: argument checks plus one queued op each.
-- Row 28 is scratch.
add("ScreenOut", N_SCREEN, function()
    for i = 0, N_SCREEN - 1 do
        ScreenOut(i % 40, 28, 32 + (i % 64), 0)
    end
    return 0
end)

-- A full collection with a live set of a hundred small tables (~15 KB
-- on the board, more on a 64-bit host): the stall a game would see if
-- the collector runs mid-frame.
add("gc full", 1, function()
    local live = {}
    for i = 1, 100 do
        live[i] = { i, i * 2, name = "obj" .. i }
    end
    collectgarbage("collect")
    local t0 = TimeNow()
    collectgarbage("collect")
    gc_stall_ms = TimeNow() - t0
    return #live
end)

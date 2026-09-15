-- The line index and the streaming line reader.

-- Checkpoints are packed 16 at a time: the OS's Lua keeps the C stack
-- small, so a long list cannot go through table.unpack in one call.
local PACK16 = "<" .. string.rep("I4", 16)

local function add_checkpoint(offset)
    npending = npending + 1
    pending[npending] = offset
    ncp = ncp + 1
    if npending == 64 then
        blocks[#blocks + 1] = string.pack(PACK16, table.unpack(pending, 1, 16))
            .. string.pack(PACK16, table.unpack(pending, 17, 32))
            .. string.pack(PACK16, table.unpack(pending, 33, 48))
            .. string.pack(PACK16, table.unpack(pending, 49, 64))
        npending = 0
    end
end

local function checkpoint(j)
    local i = j - 1
    local b = i // 64 + 1
    if b <= #blocks then
        return (string.unpack("<I4", blocks[b], (i % 64) * 4 + 1))
    end
    return pending[i % 64 + 1]
end

local function line_start_seen(offset)
    started = started + 1
    last_start = offset
    if (started - 1) % STRIDE == 0 then add_checkpoint(offset) end
end

local function count_lines()
    if indexing or truncated then return math.max(started - 1, 0) end
    if started > 0 and last_start >= size then return started - 1 end
    return started
end

-- Index up to `budget_ms` of chunks; lines are counted exactly, every
-- STRIDE-th start is remembered.
local function index_some(budget_ms)
    local t0 = TimeNow()
    while indexing do
        if index_pos >= size or started >= MAX_LINES then
            truncated = index_pos < size
            indexing = false
            break
        end
        file:seek(index_pos)
        local chunk = file:read(CHUNK)
        if not chunk or #chunk == 0 then
            indexing = false
            break
        end
        local from = 1
        while true do
            local nl = chunk:find("\n", from, true)
            if not nl then break end
            line_start_seen(index_pos + nl)
            if started >= MAX_LINES then break end
            from = nl + 1
        end
        index_pos = index_pos + #chunk
        if TimeNow() - t0 >= budget_ms then break end
    end
    lines = count_lines()
    return not indexing
end

-- A line as shown: tabs expanded, clipped to the view.
local function clip(text)
    text = text:gsub("\r$", ""):gsub("\t", TAB)
    return text:sub(hscroll + 1, hscroll + COLS)
end

-- Walk lines from the checkpoint at or before `first`, calling
-- fn(k, prefix, offset) for lines first..last with each line's first
-- `keep` bytes and its start offset. Long lines are never held whole.
local function walk(first, last, keep, fn)
    local j = (first - 1) // STRIDE
    local k = j * STRIDE + 1
    local pos = checkpoint(j + 1)
    local line_pos = pos
    local acc = ""
    while k <= last do
        file:seek(pos)
        local chunk = file:read(CHUNK)
        if not chunk or #chunk == 0 then break end
        local from = 1
        while k <= last do
            local nl = chunk:find("\n", from, true)
            if k >= first and #acc < keep then
                acc = acc .. chunk:sub(from, math.min((nl or #chunk + 1) - 1, from + keep - #acc - 1))
            end
            if not nl then break end
            if k >= first then fn(k, acc, line_pos) end
            acc = ""
            k = k + 1
            from = nl + 1
            line_pos = pos + nl
        end
        pos = pos + #chunk
    end
    if k <= last and k >= first then fn(k, acc, line_pos) end -- last line, no newline
end

-- Lines first..first+count-1 as clipped strings (nil past the end).
local function read_lines(first, count)
    local out = {}
    local last = math.min(first + count - 1, lines)
    if last >= first then
        walk(first, last, hscroll + COLS, function(k, text)
            out[k - first + 1] = clip(text)
        end)
    end
    return out
end

-- The start offset of line k.
local function line_start(k)
    if k > lines then return size end
    local offset = size
    walk(k, k, 0, function(_, _, at) offset = at end)
    return offset
end

-- The line containing byte `offset`: the checkpoint before it, then
-- count the newlines in between.
local function line_at(offset)
    local lo, hi = 1, ncp
    while lo < hi do
        local mid = (lo + hi + 1) // 2
        if checkpoint(mid) <= offset then lo = mid else hi = mid - 1 end
    end
    local k = (lo - 1) * STRIDE + 1
    local pos = checkpoint(lo)
    while pos < offset do
        file:seek(pos)
        local chunk = file:read(math.min(CHUNK, offset - pos))
        if not chunk or #chunk == 0 then break end
        local from = 1
        while true do
            local nl = chunk:find("\n", from, true)
            if not nl then break end
            k = k + 1
            from = nl + 1
        end
        pos = pos + #chunk
    end
    return math.min(k, math.max(lines, 1))
end

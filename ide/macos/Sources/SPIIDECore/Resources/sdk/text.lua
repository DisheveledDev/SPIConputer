-- SDK: Text
-- Summary: String helpers for a 40-column screen: alignment, padding, wrapping, numbers.
-- Namespaces: Text
--
-- Same format contract as screen.lua (preamble, `function Name.Sub`
-- blocks stripped when unused, `---` signature lines).
--
-- Everything here returns strings or arrays of strings; the Screen and
-- Overlay frameworks draw them. Widths are in cells (bytes).

Text = Text or {}

--- Text.Center(s, width [, fill])
-- `s` centred in `width` cells, padded with `fill` (default space);
-- longer strings are returned unchanged.
function Text.Center(s, width, fill)
    s = tostring(s)
    fill = fill or " "
    local space = width - #s
    if space <= 0 then return s end
    local left = space // 2
    return string.rep(fill, left) .. s .. string.rep(fill, space - left)
end

--- Text.Left(s, width [, fill])
-- `s` padded on the right to `width` cells (left-aligned).
function Text.Left(s, width, fill)
    s = tostring(s)
    if #s >= width then return s end
    return s .. string.rep(fill or " ", width - #s)
end

--- Text.Right(s, width [, fill])
-- `s` padded on the left to `width` cells (right-aligned).
function Text.Right(s, width, fill)
    s = tostring(s)
    if #s >= width then return s end
    return string.rep(fill or " ", width - #s) .. s
end

--- Text.Truncate(s, width [, ellipsis])
-- `s` cut to `width` cells, ending in `ellipsis` (default "..") when
-- it had to be cut.
function Text.Truncate(s, width, ellipsis)
    s = tostring(s)
    if #s <= width then return s end
    ellipsis = ellipsis or ".."
    if width <= #ellipsis then return s:sub(1, width) end
    return s:sub(1, width - #ellipsis) .. ellipsis
end

--- Text.Wrap(s, width)
-- Word-wraps `s` to lines of at most `width` cells; words longer than
-- the width are split. Newlines in `s` start new lines. Returns an
-- array of lines.
function Text.Wrap(s, width)
    local lines = {}
    for paragraph in (tostring(s) .. "\n"):gmatch("([^\n]*)\n") do
        local line = ""
        for token in paragraph:gmatch("%S+") do
            local word = token -- loop variables are const in Lua 5.5
            while #word > width do
                if #line > 0 then
                    lines[#lines + 1] = line
                    line = ""
                end
                lines[#lines + 1] = word:sub(1, width)
                word = word:sub(width + 1)
            end
            if #line == 0 then
                line = word
            elseif #line + 1 + #word <= width then
                line = line .. " " .. word
            else
                lines[#lines + 1] = line
                line = word
            end
        end
        lines[#lines + 1] = line
    end
    return lines
end

--- Text.Lines(s)
-- Splits `s` on newlines into an array of lines.
function Text.Lines(s)
    local lines = {}
    for line in (tostring(s) .. "\n"):gmatch("([^\n]*)\n") do
        lines[#lines + 1] = line
    end
    return lines
end

--- Text.Split(s [, sep])
-- Splits `s` on `sep` (a plain string, default: runs of whitespace).
function Text.Split(s, sep)
    s = tostring(s)
    local parts = {}
    if not sep then
        for word in s:gmatch("%S+") do parts[#parts + 1] = word end
        return parts
    end
    local start = 1
    while true do
        local i, j = s:find(sep, start, true)
        if not i then
            parts[#parts + 1] = s:sub(start)
            return parts
        end
        parts[#parts + 1] = s:sub(start, i - 1)
        start = j + 1
    end
end

--- Text.Join(list [, sep])
-- Joins an array of values into one string with `sep` (default " ").
function Text.Join(list, sep)
    local parts = {}
    for i, v in ipairs(list) do parts[i] = tostring(v) end
    return table.concat(parts, sep or " ")
end

--- Text.Trim(s)
-- `s` without leading and trailing whitespace.
function Text.Trim(s)
    return (tostring(s):gsub("^%s+", ""):gsub("%s+$", ""))
end

--- Text.StartsWith(s, prefix)
-- True when `s` begins with `prefix`.
function Text.StartsWith(s, prefix)
    return tostring(s):sub(1, #prefix) == prefix
end

--- Text.EndsWith(s, suffix)
-- True when `s` ends with `suffix`.
function Text.EndsWith(s, suffix)
    return #suffix == 0 or tostring(s):sub(-#suffix) == suffix
end

--- Text.Zero(n, width)
-- Integer `n` zero-padded to `width` digits: Text.Zero(7, 3) is "007".
function Text.Zero(n, width)
    return string.format("%0" .. width .. "d", math.floor(n))
end

--- Text.Commas(n)
-- Integer `n` with thousands separators: Text.Commas(1234567) is
-- "1,234,567".
function Text.Commas(n)
    local s = tostring(math.floor(n))
    local sign = ""
    if s:sub(1, 1) == "-" then sign, s = "-", s:sub(2) end
    local out = s:reverse():gsub("(%d%d%d)", "%1,"):reverse()
    if out:sub(1, 1) == "," then out = out:sub(2) end
    return sign .. out
end

--- Text.Plural(n, singular [, plural])
-- "1 file" / "3 files": the count and the right word.
function Text.Plural(n, singular, plural)
    if n == 1 then return n .. " " .. singular end
    return n .. " " .. (plural or (singular .. "s"))
end

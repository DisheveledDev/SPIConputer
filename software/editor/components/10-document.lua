-- Document: the RAM buffer is an array of lines; the SD card only sees
-- whole-file reads and writes.

local function load_content(content)
    if content == "" then return { "" } end
    local base = content
    if content:sub(-1) ~= "\n" then base = content .. "\n" end
    local t, i = {}, 0
    for l in base:gmatch("([^\n]*)\n") do i = i + 1; t[i] = l end
    return t
end

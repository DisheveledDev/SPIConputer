-- Command flow: submit a line, finish a command (READY. or the next
-- script line), and start-up.

-- Output is on screen: run the next script line, or take input again.
local script_ready = false

local function resume()
    if #script > 0 then
        script_ready = true
    else
        start_input()
    end
end

local function finish_command()
    end_redirect()
    if #script == 0 then out("READY.") end
    if flush() then resume() end
end

local function submit(line)
    commit_input()
    page_count = 0
    execute(line)
    if not waiting_child then finish_command() end
end

-- The shell prints a program's result: a message; or a table whose
-- `message` prints first, then its `lines` in order, then every other
-- field as KEY = VALUE. A failure goes to the screen even when redirected.
local function show_utility_result(ok, result)
    -- A program may end by asking the shell to run another one (the APPS
    -- launcher does): { run = path, kind = kind [, args = {...}] }.
    if ok and type(result) == "table" and type(result.run) == "string" then
        run_program(result.run, type(result.args) == "table" and result.args or {},
                    result.kind or "application", {})
        return
    end
    if type(result) ~= "table" then
        if ok then out(result) else out_error(result) end
        return
    end
    if result.message ~= nil then
        if ok then out(result.message) else out_error(result.message) end
    end
    -- The lines are queued as one producer walking the table, so a long
    -- result does not also grow the output queue line by line.
    local lines = result.lines
    if type(lines) == "table" then
        local i = 0
        emit({ next = function()
            i = i + 1
            return lines[i] ~= nil and tostring(lines[i]) or nil
        end })
    end
    local keys = {}
    for key in pairs(result) do
        if key ~= "message" and key ~= "lines" then keys[#keys + 1] = key end
    end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    for _, key in ipairs(keys) do
        out(string.format("%s = %s", tostring(key):upper(), tostring(result[key])))
    end
end

local function banner()
    out("")
    out("     **** SPIComputer OS v" .. VERSION .. " ****")
    out("")
    local ok, free = pcall(fs.free)
    free = (ok and type(free) == "number") and free or 0
    if free >= 1024 then
        out(string.format(" %s   SD CARD %s MB FREE", _VERSION, Text.Commas(free // 1024)))
    else
        out(string.format(" %s   SD CARD %s KB FREE", _VERSION, Text.Commas(free)))
    end
    out("")
end

function setup()
    Screen.Mode(1) -- 40x30 tiles, invert + colour
    default_colours()
    banner()
    -- /data/autoexec.txt runs at start-up, like EXEC.
    if fs.exists("/data/autoexec.txt") then
        execute("exec /autoexec.txt")
    end
    finish_command()
end

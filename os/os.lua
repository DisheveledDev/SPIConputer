-- os.lua
-- SPIComputer OS shell: the boot program (pid 0).
-- Copy this file to the root of the SD card.
--
-- Every program implements setup()/tick()/finish(); tick() reads input
-- with InputPoll() and runs commands.

local line = ""

local function log(msg)
    local f = fs.open("boot.log", "a")
    if f then f:write(msg) f:close() end
end

local function exec(cmd)
    local name, arg = cmd:match("^(%S+)%s*(.*)$")
    if not name then return end
    if name == "dir" or name == "ls" then
        for _, e in ipairs(fs.ls("/")) do
            local kind = e.dir and "dir " or "file"
            print(string.format("  %-4s %-24s %d bytes", kind, e.name, e.size))
        end
    elseif name == "run" then
        local prog, parg = arg:match("^(%S+)%s*(.*)$")
        if not prog then
            print("usage: run <program> [arg]")
        elseif parg == "" then
            local ok, err = Launch(prog)
            if not ok then print("launch failed: " .. tostring(err)) end
        else
            local ok, err = Launch(prog, parg)
            if not ok then print("launch failed: " .. tostring(err)) end
        end
    elseif name == "edit" then
        if arg == "" then
            print("usage: edit <file>")
        else
            local ok, err = Launch("editor.lua", arg)
            if not ok then print("launch failed: " .. tostring(err)) end
        end
    elseif name == "quit" or name == "exit" then
        print("bye")
        ExitProgram()
    elseif name == "help" then
        print("commands: dir, run <program> [arg], edit <file>, quit")
    else
        print("unknown command: " .. name)
    end
end

function setup()
    print("SPIComputer OS shell (pid " .. Pid() .. ")")
    print("Lua " .. _VERSION)
    print("type 'help' for commands")
end

function tick()
    while true do
        local ev = InputPoll()
        if not ev then break end
        if ev.type == "key" and ev.pressed == 1 then
            local k = ev.key
            if k == 13 then
                print("> " .. line)
                exec(line)
                line = ""
            elseif k == 8 then
                line = line:sub(1, -2)
            elseif k >= 32 and k < 128 then
                line = line .. string.char(k)
            end
        end
    end
end

function finish()
    print("shell exiting")
    log("shell-finish\n")
end

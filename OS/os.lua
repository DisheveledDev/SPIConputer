-- os.lua
-- SPIComputer OS boot script. Copy this file to the root of the SD card.
-- Runs automatically at boot (see fatfs_lua.c / SPIComputerOS.c).

print("SPIComputer OS boot")
print("Lua " .. _VERSION .. " on RP2350 (Pico 2)")

local fs = require("fs")

if not fs.ready() then
    print("SD card not mounted")
    return
end

local free_kb, total_kb = fs.free()
print(string.format("SD card: %d KB free of %d KB", free_kb, total_kb))

print("Root directory:")
for _, e in ipairs(fs.ls("/")) do
    local kind = e.dir and "dir " or "file"
    print(string.format("  %-4s %-24s %d bytes", kind, e.name, e.size))
end

-- Demo: append a line to boot.log to prove the write path works
local f, err = fs.open("boot.log", "a")
if f then
    f:write("boot ok\n")
    f:close()
    print("Wrote boot.log")
else
    print("Could not open boot.log: " .. err)
end

print("Boot complete")

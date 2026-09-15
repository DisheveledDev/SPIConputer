-- hello.lua — an example command-line utility.
--
-- Installed as utils/hello.util it becomes the shell command HELLO:
--
--   HELLO one two
--   HELLO FROM A UTILITY
--   ARGS = 2
--   FIRST = one
--
-- A utility has no screen or sound of its own. It runs once, reads the
-- words typed after its name from `args`, and hands a table back to the
-- shell with UtilityResult: `message` prints first, every other field
-- prints as KEY = VALUE.

function setup()
    UtilityResult(true, {
        message = "HELLO FROM A UTILITY",
        args = #args,
        first = args[1],
    })
end

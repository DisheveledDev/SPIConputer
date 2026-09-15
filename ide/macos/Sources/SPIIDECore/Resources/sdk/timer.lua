-- SDK: Timer
-- Summary: Timer objects you can cancel, pause, resume and restart; one-shots and repeats.
-- Namespaces: Timer
--
-- Same format contract as screen.lua (preamble, `function Name.Sub`
-- blocks stripped when unused, `---` signature lines). Methods on timer
-- objects (`t:Pause()`) dispatch to `Timer.Pause(t)`, and the builder
-- keeps a block when the program calls it either way.
--
-- Built on the OS timers (TimerCreate/TimerStop, 8 per program, 1 ms
-- resolution). Pause and resume are done here: a paused timer's OS
-- timer is stopped and the time left until its next fire is kept, so
-- resuming continues where it stopped rather than restarting.
-- Callbacks receive the timer object, so a handler can cancel or
-- pause its own timer. Callbacks run from the scheduler between ticks,
-- never in the middle of one.

Timer = Timer or {}

local timers = {}   -- id -> timer object, while it can still fire
local next_id = 0

-- t.id          number, unique within the program
-- t.interval    ms between fires
-- t.oneshot     true for After
-- t.fn          the callback
-- t.os_id       OS timer id while armed (nil when paused, done or cancelled)
-- t.armed_at    TimeNow() when the current OS timer was armed
-- t.remaining   ms left until the next fire while paused
-- t.catching_up true while a resumed repeating timer waits out its
--               remaining time on a one-shot before re-arming
-- t.done        true once a one-shot has fired or any timer is cancelled

local timer_mt = {
    __index = function(_, key)
        return Timer[key]
    end,
    __tostring = function(t)
        return "Timer(" .. t.id .. ")"
    end,
}

-- A timer object from an object or an id; nil for anything else.
local function resolve(t)
    if type(t) == "table" then return t end
    if type(t) == "number" then return timers[t] end
    return nil
end

-- What happens when the OS timer behind `t` fires.
local function fire(t)
    if t.done then return end
    if t.oneshot then
        t.done = true
        t.os_id = nil
        timers[t.id] = nil
        t.fn(t)
        return
    end
    if t.catching_up then
        -- The remaining-time one-shot has fired: back to the interval.
        t.catching_up = false
        t.os_id = TimerCreate(function() fire(t) end, t.interval, false)
    end
    t.armed_at = TimeNow()
    t.fn(t)
end

-- Arm the OS timer for `t`: `first` ms until the first fire (the
-- interval, or what was left when it was paused).
local function arm(t, first)
    if t.oneshot or first ~= t.interval then
        t.catching_up = not t.oneshot
        t.os_id = TimerCreate(function() fire(t) end, first, true)
    else
        t.catching_up = false
        t.os_id = TimerCreate(function() fire(t) end, t.interval, false)
    end
    t.armed_at = TimeNow()
    t.remaining = nil
end

--- Timer.Create(fn, ms [, oneshot])
-- A timer calling fn(timer) every `ms` milliseconds, or once when
-- `oneshot` is true. Returns the timer object (its `id` field is the
-- number Timer.Get and the other calls accept). Raises an error when the
-- program's 8 OS timers are all in use.
function Timer.Create(fn, ms, oneshot)
    if type(fn) ~= "function" then error("Timer.Create: fn must be a function", 2) end
    ms = math.max(1, math.floor(tonumber(ms) or 1))
    next_id = next_id + 1
    local t = setmetatable({
        id = next_id, interval = ms, oneshot = oneshot and true or false, fn = fn,
    }, timer_mt)
    timers[t.id] = t
    arm(t, ms)
    return t
end

--- Timer.After(ms, fn)
-- Calls fn(timer) once, `ms` milliseconds from now. Returns the timer.
function Timer.After(ms, fn)
    return Timer.Create(fn, ms, true)
end

--- Timer.Every(ms, fn)
-- Calls fn(timer) every `ms` milliseconds until cancelled. Returns the timer.
function Timer.Every(ms, fn)
    return Timer.Create(fn, ms, false)
end

--- Timer.Cancel(timer)
-- Stops a timer for good (object or id). Returns true if it was live.
function Timer.Cancel(timer)
    local t = resolve(timer)
    if not t or t.done then return false end
    if t.os_id then TimerStop(t.os_id) end
    t.os_id = nil
    t.done = true
    timers[t.id] = nil
    return true
end

--- Timer.Pause(timer)
-- Stops the clock on a running timer, remembering how long was left
-- until its next fire. Returns true if it was running.
function Timer.Pause(timer)
    local t = resolve(timer)
    if not t or t.done or not t.os_id then return false end
    local elapsed = TimeNow() - t.armed_at
    local due = t.catching_up and (t.remaining or t.interval) or t.interval
    t.remaining = math.max(1, due - elapsed)
    TimerStop(t.os_id)
    t.os_id = nil
    return true
end

--- Timer.Resume(timer)
-- Restarts a paused timer with the time that was left when it was
-- paused. Returns true if it was paused.
function Timer.Resume(timer)
    local t = resolve(timer)
    if not t or t.done or t.os_id or not t.remaining then return false end
    arm(t, t.remaining)
    return true
end

--- Timer.Restart(timer [, ms])
-- Re-arms a timer from now with its interval (or a new one), whether it
-- was running, paused or already fired. Returns the timer.
function Timer.Restart(timer, ms)
    local t = resolve(timer)
    if not t then return nil end
    if t.os_id then TimerStop(t.os_id) end
    if ms then t.interval = math.max(1, math.floor(ms)) end
    t.done = false
    timers[t.id] = t
    arm(t, t.interval)
    return t
end

--- Timer.Running(timer)
-- True while the timer is armed (not paused, cancelled or fired).
function Timer.Running(timer)
    local t = resolve(timer)
    return t ~= nil and not t.done and t.os_id ~= nil
end

--- Timer.Paused(timer)
-- True while the timer is paused.
function Timer.Paused(timer)
    local t = resolve(timer)
    return t ~= nil and not t.done and t.os_id == nil and t.remaining ~= nil
end

--- Timer.Remaining(timer)
-- Milliseconds until the next fire (frozen while paused), or nil when
-- the timer is done.
function Timer.Remaining(timer)
    local t = resolve(timer)
    if not t or t.done then return nil end
    if t.remaining and not t.os_id then return t.remaining end
    local due = t.catching_up and (t.remaining or t.interval) or t.interval
    return math.max(0, due - (TimeNow() - t.armed_at))
end

--- Timer.Get(id)
-- The live timer object with that id, or nil.
function Timer.Get(id)
    return timers[id]
end

--- Timer.CancelAll()
-- Cancels every live timer this framework created. Returns how many.
function Timer.CancelAll()
    local n = 0
    for id, t in pairs(timers) do
        if t.os_id then TimerStop(t.os_id) end
        t.os_id = nil
        t.done = true
        timers[id] = nil
        n = n + 1
    end
    return n
end

--- Timer.Count()
-- How many timers are live (running or paused).
function Timer.Count()
    local n = 0
    for _ in pairs(timers) do n = n + 1 end
    return n
end

# Lua API Reference — SPIComputer OS

The contract between OS programs and the runtime. This document is the
single reference for app developers and for the (future) IDE; keep it in
sync with the implementation (`sys_lua.c`, `fs_lua.c`, `program.c`).
Anything not listed here is not part of the supported API.

## Program Structure

A program is a Lua file on the SD card. The file body runs once at
launch and must only **define** the entry points (plus any local state);
it must not block or run the main logic:

```lua
-- myprog.lua
local ticks = 0            -- program-local state persists between ticks

function setup()           -- optional: called once when the program starts
    print("hello")
end

function tick()            -- called repeatedly, as fast as possible
    ticks = ticks + 1
end

function finish()          -- optional: called when the program exits
    print("goodbye")
end
```

Semantics:

- `setup()` is called once, before the first `tick()`.
- `tick()` runs free-running (no arguments, no dt; use `TimeNow()` to
  compute elapsed time yourself). One scheduler step runs either due
  timer callbacks **or** one `tick()` — they never overlap or re-enter.
- `finish()` runs when the program exits (see `ExitProgram()`).
- If `setup()` throws, the program never starts and `Launch()` reports
  the error to its caller.
- If `tick()` or a timer callback throws, the error is printed and the
  program is terminated (after `finish()`); the program that launched it
  resumes on its next tick.
- `print()` goes to the standard output (serial/USB console).
- A global `pid` (number) is set at launch, plus `Pid()`.

## OS Functions

Available as globals in every program.

### Time and process

| Function | Returns |
|---|---|
| `TimeNow()` | milliseconds since boot (monotonic) |
| `Pid()` | this program's pid (number) |
| `ExitProgram()` | nothing; asks the OS to exit after the current tick/timer callback returns |
| `Launch(path [, arg])` | `true`, or `nil, err` on failure (missing file, Lua error in body/setup, too many programs, out of memory). On success the caller is **paused**; it resumes when the launched program exits. `arg` (a string) is passed to the program chunk as its first vararg: `local filename = ...` |

### Timers

| Function | Returns |
|---|---|
| `TimerCreate(fn, interval_ms [, oneshot])` | timer id (>= 1), or raises an error if the program has too many timers |
| `TimerStop(id)` | `true` if the timer existed and was stopped |

- `fn` takes no arguments; it runs in the program's own state.
- Minimum interval is 1 ms.
- `oneshot` defaults to `false` (repeating).
- Deadlines are absolute; while the program is paused (another program
  on top) its timers pause too, and deadlines shift by the pause
  duration on resume (no catch-up bursts).
- Maximum 8 timers per program.

### Input

| Function | Returns |
|---|---|
| `InputPoll()` | the next pending input event as a table, or `nil` when none |
| `InputControl(n)` | current joystick state for controller `n` (1 or 2) as `{up=, down=, left=, right=, fire=}` (booleans), or `nil` for invalid `n` |

Event table shape:

```lua
{ type = "key" | "control1" | "control2",
  key = <number>,      -- ASCII code, or extended code (see below)
  mods = <number>,     -- bitmask of modifiers held
  pressed = 0 | 1,     -- 1 = down, 0 = up
  ctrl = <number>,     -- controller number for control events
  dirs = <number> }    -- bitmask of changed direction(s) for control events
```

Typical reading pattern inside `tick()`:

```lua
while true do
    local ev = InputPoll()
    if not ev then break end
    if ev.type == "key" and ev.pressed == 1 then
        if ev.key == 13 then ... end          -- Return
    end
end
```

Key codes:

- Printable keys carry their ASCII code (letters arrive lowercase
  unshifted, uppercase when Shift is held — the shift is already applied
  to `key`; `mods` also records it).
- Control keys: Return = 13, Tab = 9, Backspace = 8, Delete = 127,
  Escape = 27, Ctrl+letter = 1-26.
- Keys with no ASCII code use the extended codes:

| Code | Key |
|---|---|
| 128 | Cursor up |
| 129 | Cursor down |
| 130 | Cursor left |
| 131 | Cursor right |
| 132-138 | F1-F7 |
| 139 | Home |
| 140 | Run/Stop |

- Modifier bits (`mods`): SHIFT = 1, CTRL = 2, C= = 4, RESTORE = 8.
  Modifier key presses also arrive as events with `key = 0` and the
  modifier bit in `mods`.
- Joystick direction bits (`dirs`): UP = 1, DOWN = 2, LEFT = 4,
  RIGHT = 8, FIRE = 16. One event per direction edge.

## Display (Screen API)

Available as globals; they operate on the program's own video state
(restored automatically when the program resumes after a launch).

### Screen modes

| Mode | Geometry | Type | Colour |
|---|---|---|---|
| 0 | 40x30 tiles | 8x8 tiles + attribute map | B&W (invert attr applies) |
| 1 | 40x30 tiles | 8x8 tiles + attribute map | per-cell invert + 7 colours |
| 2 | 80x60 tiles | 8x8 tiles + attribute map | B&W |
| 3 | 80x60 tiles | 8x8 tiles + attribute map | per-cell invert + 7 colours |
| 10 | 320x240 | direct pixels | 256-entry palette |

Output is always 640x480; modes 0/1/10 are scaled 2x. The RP2040 dev
board supports modes 0 and 1 only.

### Functions

| Function | Returns |
|---|---|
| `ScreenMode(mode)` | `true`, or `nil, err` (invalid mode, unsupported board, out of memory for mode 10) |
| `ScreenOut(x, y, char [, attr])` | `true`, or `nil, err` (text modes) |
| `ScreenAttr(x, y, flags)` | `true`, or `nil, err` |
| `ScreenDefineTile(index, bytes)` | `true` (bytes = table of 8 numbers or 8-byte string) |
| `ScreenPalette(i, r, g, b)` | `true` |
| `ScreenPaletteSet(t)` | `true` (t = array of `{r,g,b}` tables or 0xRRGGBB integers, up to 256) |
| `ScreenClear([char])` | `true` (defaults to space) |
| `ScreenPlot(x, y, colour)` | `true`, or `nil, err` (mode 10 only) |

Attribute byte: bit 7 = invert (swap fg/bg), bits 0-2 = colour index.
Colour index `c` uses palette entry `c+1` (so 0 = default white); the
background is palette entry 0 (black). Tiles not redefined render with
the ROM font (ASCII-aligned, tile index = character code). A mode
switch clears the screen; switching to mode 10 allocates its
framebuffer (only one mode 10 program may run; `Launch` from mode 10
fails).

## Sound and Music (Sound API)

Available as globals; they operate on the program's own audio state
(restored automatically when the program resumes after a launch). The
engine renders 8 score channels plus 8 sound-effect voices, stereo, at
44.1 kHz. The mix is designed to feed the HDMI audio data islands; the
HSTX output backend lands with the Phase 7 hardware bring-up, so audio is
not audible on the RP2040 dev board.

### Sounds and samples

| Function | Returns |
|---|---|
| `SoundDefine(id, spec)` | `true`, or `nil, err` (id = 0..31) |
| `SoundLoad(path)` | sound id (32..39), or `nil, err` |
| `SoundPlay(sound [, note [, dur_ms [, vol [, pan]]]])` | voice id (1..8), or `nil, err` |
| `SoundStop([voice])` | `true`/`false` (no argument: all one-shots) |
| `SoundStopAll()` | `true` (stops the score too) |
| `SoundVolume(v)` | `true` (master 0..255) |

`SoundDefine` spec: `wave` (`"square"`/`"pulse"`, `"triangle"`, `"saw"`,
`"sine"`, `"noise"`), `duty` (1..15, pulse width in 16ths), `attack`,
`decay`, `release` (ms, 0..255), `sustain` (level 0..255), `volume`
(0..255). All fields are optional.

`note` is a name (`"C4"` = middle C, `"A#3"`, `"Bb3"`, `"C-1"`..`"G9"`) or
a MIDI number 0..127 (default C4). `dur_ms` holds the envelope before
release; omit/0 = one-shot (release after attack+decay). `vol` 0..255,
`pan` -64 (left) .. 63 (right).

`SoundLoad` parses a WAV file (PCM, 8/16-bit, mono/stereo, rate up to
48 kHz) from the SD card into the program's 64 KB sample pool and returns
a sound id (32+n) usable with `SoundPlay` and in scores. Samples play
pitch-shifted (C4 = original rate).

### Scores

| Function | Returns |
|---|---|
| `MusicDefine(name, spec)` | `true`, or `nil, err` |
| `MusicPlay(name [, loop])` | `true`, or `nil, err` |
| `MusicStop()` | `true` |
| `MusicPlaying()` | boolean |

`MusicDefine` spec: `loop` (default false) and `channels`, an array of up
to 8 channels, each an array of events:

```lua
MusicDefine("tune", {
  loop = true,
  channels = {
    { {at=0,   sound=0, note="C4", dur=200, vol=255, pan=0},
      {at=250, sound=0, note="E4", dur=200} },
    { {at=0,   sound=1, note="C2", dur=100} },
  },
})
MusicPlay("tune")
```

Event fields: `at` (ms from the start), `sound` (id, required), `note`,
`dur` (ms; omit/0 = one-shot envelope), `vol` (0..255), `pan` (-64..63).
Events may be written in any order (they are sorted per channel).
`MusicPlay` overrides the score's own `loop` when the second argument is
given. `MusicPlaying()` is true from the call to `MusicPlay` until the
score ends or `MusicStop`/`SoundStopAll`; a paused program is silenced
and resumes from its score position.

## The `fs` Module

SD card filesystem, loaded with `local fs = require("fs")`.

### File operations

| Function | Returns |
|---|---|
| `fs.open(path [, mode])` | file object, or `nil, err` |
| `fs.ls([path])` | array of `{name=, size=, dir=}` |
| `fs.stat(path)` | `{size=, dir=}`, or `nil, err` |
| `fs.exists(path)` | boolean |
| `fs.mkdir(path)` | `true`, or `nil, err` |
| `fs.remove(path)` | `true`, or `nil, err` |
| `fs.rename(old, new)` | `true`, or `nil, err` |
| `fs.free()` | `free_kb, total_kb` |
| `fs.ready()` | boolean (SD card mounted) |
| `fs.mount()` | boolean (re-mount attempt) |
| `fs.readall(path)` | whole file as a string, or `nil, err` (128 KB cap) |
| `fs.writeall(path, data)` | `true`, or `nil, err` |

Open modes: `"r"` read, `"w"` write (truncate), `"a"` append, `"+"`
upgrade to read+write. Default is `"r"`.

### File objects

| Method | Returns |
|---|---|
| `f:read(n)` | string (shorter than `n` at EOF) |
| `f:write(s)` | bytes written |
| `f:seek(offset [, "set"|"cur"|"end"])` | new position |
| `f:tell()` | position |
| `f:size()` | file size |
| `f:flush()` | `true` |
| `f:close()` | `true` |

Files are also closed automatically when garbage-collected or when the
program exits. Note: all fs calls block the program until core 0 has
serviced them (they are RPCs); a missing or ejected SD card produces
`nil, err`, never a crash.

## Loading Code from the SD Card

- `loadfile(path)` / `dofile(path)` — replaced with SD-backed versions;
  the file is read from the card and compiled on core 1.
- `require("name")` — an SD-backed searcher (slot 2) looks for
  `name`, `name.lua`, `/name`, `/name.lua`, `/lib/name`, `/lib/name.lua`
  in that order.

The standard Lua 5.5 libraries are available: base, coroutine, table,
string, math, utf8, package.

## Limits and Semantics

| Limit | Value |
|---|---|
| Program stack depth | 4 programs (including the shell) |
| Lua heap per program | 64 KB (allocation failure raises a Lua memory error, which terminates the program) |
| Script size | 128 KB |
| Timers per program | 8 |
| Pending input events per program | 128 (older events dropped) |
| Sound definitions per program | 32 |
| Scores per program | 8 (shared pool of 1024 events) |
| WAV sample pool per program | 64 KB (8 samples max) |

- `Launch()` from a program pauses it; input arriving while paused goes
  to whichever program is on top when the scheduler drains the queue.
- Programs cannot call each other's functions; each has its own
  `lua_State`. The only cross-program interaction is launch/exit.
- There is no `os.exit`/`os.time` style API — use `ExitProgram()` and
  `TimeNow()`.
- The shell (`os.lua`) is just another program (pid 0).

## Not Yet Implemented

The HDMI/HSTX video output path and the HDMI audio data-island output
backend (hardware bring-up, Phase 7). Programs run fully on the dev board
with the serial mirror as the display; the sound engine itself is
complete and host-tested meanwhile.

## Built-in Programs

- `os.lua` — the shell (pid 0): `dir`, `run <program> [arg]`,
  `edit <file>`, `quit`. Input comes from the keyboard matrix or the
  terminal app over RS232.
- `editor.lua` — the text editor: `edit <file>` from the shell (or
  `run editor.lua <file>`). Edits in RAM, saves with Ctrl+S, quits with
  Ctrl+Q (asks first when the buffer is dirty). Cursor keys move (both
  the extended key codes and the terminal's ESC [ A/B/C/D sequences
  work); Backspace deletes, Shift+Backspace inserts a space, Forward
  delete deletes at the cursor, Home jumps to the start of the line.

## Example

```lua
-- blink.lua: flashes a marker in the shell log every 500 ms until a
-- key is pressed.
local function log(msg)
    local f = fs.open("boot.log", "a")
    if f then f:write(msg) f:close() end
end

function setup()
    TimerCreate(function() log("blink\n") end, 500)
end

function tick()
    while InputPoll() do
        ExitProgram()
        return
    end
end

function finish()
    log("blink stopped\n")
end
```

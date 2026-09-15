# Lua API Reference — SPIComputer OS

The contract between OS programs and the runtime. This document is the
single reference for app developers and for the (future) IDE; keep it in
sync with the implementation (`sys_lua.c`, `fs_lua.c`, `program.c`).
Anything not listed here is not part of the supported API.

## Program Structure

A program is a Lua source file or compiled `.prg` bytecode file on the SD
card. The file body runs once at launch and must only **define** the entry
points (plus any local state);
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
| `Launch(path [, arg [, replace]])` | `true`, or `nil, err` on failure (missing file, Lua error in body/setup, too many programs, out of memory). `arg` (a string) is passed to the program chunk as its first vararg: `local filename = ...`. With `replace` true the caller leaves the program stack on its way out (see below) |
| `Execute(path, arg1, ...)` | `true`, or `nil, err`; runs a program file in the foreground with up to 16 string arguments |
| `ExecuteString(source, arg1, ...)` | `true`, or `nil, err`; compiles and runs a Lua source string the same way |
| `Compile(src [, dst])` | `true, bytes`, or `nil, err` (missing file, syntax error with line, write failure); compiles the Lua source file `src` on the card into a `.prg` binary chunk, the same format the IDE builds and `Launch`/`Execute`/`dofile`/`require` load. `dst` defaults to `src` with `.lua` replaced by `.prg`. Paths are program-relative like the `fs` module's. The parser runs outside the caller's heap cap |
| `UtilityResult(ok, message)` | ends the program with a result for its caller: every utility does; an interactive program may (a picker returning a choice) |
| `UtilityPoll()` | `ok, message`, or `nil`; retrieves the result of a child that ended with `UtilityResult` |

Launch/Execute semantics: on success the new program runs on top of
the stack and the caller stops being scheduled until it exits. The call
itself **returns immediately** (the caller's Lua resumes right away, with
the child's video/audio current), so nothing may follow the call except
returning from the current `tick()`; code that must run *after* the
child exits belongs in the caller's next `tick()`, which only happens
once the child has gone:

`Launch(path, arg, true)` replaces the caller instead of stacking on it:
the new program takes the caller's place (the caller's parent becomes its
parent) and the caller leaves the stack. Its Lua state, timers and audio
state are released once the call that launched it has returned, so a boot
loader can hand the machine to the shell without staying resident. The
caller's `finish()` still runs during that release.

```lua
local child_running = false

function tick()
    if child_running then
        -- We only tick again after the child exited.
        child_running = false
        print("the program finished")
        return
    end
    if some_condition then
        child_running = true
        Execute("child.lua", "arg")
    end
end
```

Arguments: every program (booted, launched or executed) gets a global
`args` table with the arguments as strings, `args[1]` first (`#args` is
the count). `args[1]` is also passed to the chunk as its first vararg,
so `local filename = ...` keeps working.

Noninteractive utilities set `__spi_interactive = false` in their generated
program. They run in an isolated Lua state but do not allocate or replace a
video/audio state. They call `UtilityResult(ok, value)` once, where `value`
is a string or a table (strings, numbers, booleans and nested tables with
string or integer keys survive the crossing; anything else becomes nil;
8 KB at most); the launching program receives `ok, value` through
`UtilityPoll()` after the utility exits, the table rebuilt directly in its
own state (no Lua compile, so a near-limit result costs the caller little
more than its strings). Installed in `utils/` as `name.util`, a utility
is a shell command: `HELLO one two` runs `utils/hello.util` with
`args = {"one", "two"}`. Keys typed while a utility runs go to the
interactive program below it (the shell's type-ahead), not to the utility.

An interactive program may end with `UtilityResult` too: its parent gets
the value the same way (a program that just exits leaves nothing to
poll). The APPS launcher (`core/apps.prg`) uses this to hand its choice
back to the shell.

The shell prints a result table's `message` first, then its `lines`
array in order, then every other field as `KEY = VALUE`; long output
stops at `-- MORE --`. A table with a `run` field (`{ run = path, kind =
"application"|"game"|"utility", args = {...} }`) is instead run by the
shell as if typed, so a game started that way still replaces the shell.

Shell arguments: words that name an existing data entry, or look like a
file name (only name characters, a letter, and a slash or an extension:
`notes.txt`, `games/x`), arrive as full card paths (`/data/...`);
unquoted words with `*` or `?` expand to the matching entries' paths;
options (`-n`), numbers (`2.5`, `1/3`), expressions and `"quoted words"`
arrive exactly as typed. `command > file` / `>> file` sends a built-in's
or utility's output to a file; `/data/autoexec.txt` runs at start-up.

Games are the other special kind: launched with `Launch(path, arg, true)`
by the shell, so the shell's state is released and the game has the
machine to itself. When the last program on the stack exits (a game, or
the shell itself) the device restarts (the simulator boots again), so a
game ends with `ExitProgram()` and never returns to a shell.

Every program also receives an `app` table with `app.root`, `app.program`,
`app.metadata`, and `app.resources` paths when launched from an `.app`
directory.

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
| `WaitVSync([ms])` | frames elapsed since this program's previous `WaitVSync` (blocking up to `ms`, default 100, for the next frame) |

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

### Frame synchronisation

`WaitVSync([ms])` returns the number of video frames that arrived since
this program's previous `WaitVSync` (the first call measures from program
start). With no argument it waits up to 100 ms for a frame; with `ms` it
waits up to that long; `WaitVSync(0)` polls once and returns immediately,
which suits programs that already run from a timer.

```lua
function tick()
    WaitVSync()          -- pace the tick to the 60 Hz display
    ...                  -- draw the next frame
end
```

On hardware without a display (dev boards, or a stalled scanout) the call
simply times out and returns 0, so programs must not assume frames always
advance.

### Optional input callbacks

Instead of (or as well as) polling, a program may define these globals:
the scheduler invokes them as events arrive, before the next `tick()`,
with the same error handling as `tick()` (a throwing callback terminates
the program). They are looked up by name for every event, so a program
(or a framework such as `Input.Keyboard.Callback`) may define, replace
or remove them at any time, including from `setup()`:

```lua
function on_keypress(key, shift, ctrl, cbm, restore)
    -- key-down events only (key ~= 0); releases still arrive via InputPoll
end

function on_control(index, up, down, left, right, fire)
    -- every joystick change, with the full stick state after the event;
    -- index is the port: 0 = joystick 1, 1 = joystick 2
    -- (note InputControl(n) uses 1/2)
end
```

Events are placed in the program's ring either way, so a program using
callbacks can still poll `InputPoll()` when it needs key releases, raw
edges, or modifier-key events.

## Display (Screen API)

Available as globals; they operate on the program's own screen slot
(restored automatically when the program resumes after a launch). Calls
are queued to the display core and applied at the next frame boundary
(50 Hz), so a change becomes visible on the following frame; a program
that queues faster than a frame's worth of changes is briefly blocked.

### Screen modes

| Mode | Geometry | Type | Colour |
|---|---|---|---|
| 0 | 40x30 tiles | 8x8 tiles + attribute map | B&W (invert attr applies) |
| 1 | 40x30 tiles | 8x8 tiles + attribute map | per-cell invert + 7 colours |
| 2 | 80x60 tiles | 8x8 tiles + attribute map | B&W (invert attr applies) |
| 3 | 80x60 tiles | 8x8 tiles + attribute map | per-cell invert + 7 colours |
| 10 | 320x240 | direct pixels (one byte each) | 256-entry palette |
| 11 | 160x120 | direct pixels (one byte each, shown 4x4) | 256-entry palette |

Output is always 640x480 at 60 Hz. Modes 0, 1 and 10 are 320x240
logically and scaled 2x; modes 2 and 3 draw their 8x8 cells 1:1 at the
full resolution (twice the cells per row, every output line rendered),
so they cost the display core about four times the rendering per line:
use them for text screens, not for animation. Every cell call
(`ScreenOut`, the block ops, the `Screen`/`Overlay` frameworks) takes
coordinates in the geometry of the mode the program selected, and
`Screen.Mode` updates `Screen.COLS`/`ROWS` and `Overlay.COLS`/`ROWS`.
The pixel modes are a plain byte buffer, one palette index per pixel
(mode 11's 19 KB buffer is the cheapest to draw and to display). They
have no text cells: `ScreenOut` and the block ops fail there, and text is
drawn as pixels with `ScreenPixelText`. Every pixel call is one display
op clipped by the display core, so shapes, sprites and text may run off
the edges. The default palette is 0-15 the text colours, 16-231 a 6x6x6
colour cube (16 + 36r + 6g + b, each 0-5), 232-255 a grey ramp;
`ScreenPalette` changes any entry. The Graphics framework (sprites with
rotation, flips and scaling, text, scrolling) is Lua over these calls.
The RP2040 dev board supports modes 0 and 1 only.

### Functions

| Function | Returns |
|---|---|
| `ScreenMode(mode)` | `true`, or `nil, err` (invalid mode, unsupported board) |
| `ScreenOut(x, y, char [, attr])` | `true`, or `nil, err` (text modes); draws on the base layer |
| `ScreenAttr(x, y, flags)` | `true`, or `nil, err` |
| `ScreenDefineTile(index, bytes)` | `true` (bytes = table of 8 row patterns or 8-byte string; bit 0 of a row is the leftmost pixel) |
| `ScreenPalette(i, r, g, b)` | `true` |
| `ScreenPaletteSet(t)` | `true` (t = array of `{r,g,b}` tables or 0xRRGGBB integers, up to 256) |
| `ScreenClear([char])` | `true` (defaults to space); clears the base layer |
| `ScreenBox(x, y, w, h [, style [, attr]])` | `true`, or `nil, err`; draws a frame with the ROM box-drawing characters (style 1 = single line, default; 2 = double). Needs `w` and `h` >= 2; clipped at the screen edge |
| `ScreenFill(x, y, w, h [, char [, attr]])` | `true`, or `nil, err`; writes `char` (default space) and `attr` into every cell of the rectangle; clipped at the screen edge |
| `ScreenFillAttr(x, y, w, h, attr)` | `true`, or `nil, err`; sets the attribute of every cell in the rectangle, characters unchanged |
| `ScreenWrite(x, y, text [, attr])` | `true`, or `nil, err`; writes the bytes of `text` as consecutive cells from (x, y), wrapping to the next row (so `ScreenWrite(0, 0, map)` with a 1200-byte string replaces the whole screen); with `attr`, every cell gets it too |
| `ScreenWriteAttr(x, y, attrs)` | `true`, or `nil, err`; the bytes of `attrs` become the attributes of consecutive cells from (x, y) |
| `ScreenCopy(sx, sy, w, h, dx, dy)` | `true`, or `nil, err`; copies a block (characters and attributes) so its top-left lands at (dx, dy); clipped so both fit |
| `ScreenMove(sx, sy, w, h, dx, dy [, char [, attr]])` | as `ScreenCopy`, then blanks the part of the source the block no longer covers with `char` (default space) and `attr` |
| `ScreenScroll(x, y, w, h, dx, dy [, char [, attr]])` | `true`, or `nil, err`; shifts the region's contents by (dx, dy) cells (negative = up/left); the cells uncovered get `char` and `attr` |
| `ScreenLoadImage(path, x, y [, w, h])` | reserved: always `nil, err` for now (the loader is not implemented) |
| `OverlayOut(x, y, char [, attr])` | `true`, or `nil, err` (text modes); draws on the overlay layer |
| `OverlayAttr(x, y, flags)` | `true`, or `nil, err` |
| `OverlayClear([char])` | `true` (defaults to space); blanks the overlay and hides it |
| `OverlayBox`, `OverlayFill`, `OverlayFillAttr`, `OverlayWrite`, `OverlayWriteAttr`, `OverlayCopy`, `OverlayMove`, `OverlayScroll` | as the `Screen` versions, on the overlay layer (a dialog over the base: `OverlayFill` the body, `OverlayBox` the frame, `OverlayWrite` the text; `OverlayClear` removes it) |
| `ScreenPlot(x, y, colour)` | `true`, or `nil, err` (modes 10 and 11) |
| `ScreenPixelRect(x, y, w, h, colour [, filled])` | `true`, or `nil, err`; a rectangle outline, or filled |
| `ScreenPixelLine(x0, y0, x1, y1, colour)` | `true`, or `nil, err`; a line, both ends included |
| `ScreenPixelCircle(cx, cy, r, colour [, filled])` | `true`, or `nil, err`; a circle outline, or a disc (r <= 1024) |
| `ScreenPixelScroll(x, y, w, h, dx, dy [, fill])` | `true`, or `nil, err`; shifts a region's pixels by (dx, dy), -127..127; uncovered pixels get `fill` |
| `ScreenBlit(x, y, w, h, pixels [, key])` | `true`, or `nil, err`; w*h palette bytes row by row to (x, y), pixels equal to `key` skipped; at most 8184 pixels a call |
| `ScreenPixelText(x, y, text, colour [, bg [, scale]])` | `true`, or `nil, err`; 8x8 font (or the program's tiles) as pixels, `scale` 1-4, cells painted `bg` when given; 64 characters a call |

The block calls (`Box`, `Fill`, `FillAttr`, `Write`, `WriteAttr`, `Copy`,
`Move`, `Scroll`) each queue **one** display op whatever the size of the
rectangle; the display core does the work at the frame boundary. Prefer
them to loops of `ScreenOut`: a full-screen repaint is one `ScreenWrite`
instead of 1200 ops. They are text-mode calls; the pixel modes have the
`ScreenPixel*`, `ScreenBlit` and `ScreenPlot` calls, one op each too.

### Frameworks

The IDE can inject read-only frameworks (SDKs) into a program: `Screen`,
`Overlay`, `Text`, `Timer`, `Sound`/`Music` and `Input` namespaces with
higher-level calls such as `Screen.CenterText(y, text)`,
`Screen.Move(x1, y1, x2, y2, x3, y3)`, `Overlay.Dialog(title, lines)`,
`Text.Wrap(s, width)`, `Timer.Every(ms, fn)` (returning an object with
`Cancel`/`Pause`/`Resume`), `Sound.Tone(hz, ms)` and
`Input.Keyboard.Callback(key, fn)`, and `Graphics` (sprites, shapes and
text on the pixel modes: `Graphics.Sprite`, `s:MoveTo`, `s:Turn`,
`Graphics.Text`). They are plain Lua over the API above,
selected per project in the IDE's settings, and stripped at build time to
the functions the program uses. The framework sources (and their
documentation) are `ide/macos/Sources/SPIIDECore/Resources/sdk/*.lua`.
The Screen framework also defines the `Attributes` namespace: the colours
by name (`Attributes.White`/`Red`/`Cyan`/`Purple`/`Green`/`Blue`/`Yellow`/
`Orange`, the mode-1 default palette) and `Attributes.Inverse`, to be added
for an `attr` argument (`Attributes.Red + Attributes.Inverse` is a red
block behind a space) without knowing the byte layout below.

Attribute byte: bit 7 = invert (swap fg/bg), bits 0-2 = colour index,
bit 6 = transparent overlay cell. Colour index `c` uses palette entry `c+1`
(so 0 = default white); the background is palette entry 0 (black). Text
modes have a base layer and one overlay layer. Overlay cells are hidden
until written: a cell shows when `OverlayOut`/`OverlayAttr` store an
attribute without bit 6, and `OverlayClear` hides the whole overlay
again. The renderer
composites a visible overlay cell over the base; hidden cells show the base.
Tiles not redefined render with the ROM font (ASCII-aligned, tile index =
character code). Tile rows and ROM font rows share one convention: bit 0 is
the leftmost pixel. A mode switch clears both layers; switching to mode 10
or 11 attaches the shared pixel buffer (only one pixel-mode program may
run; `Launch` from a pixel mode fails).

### ROM character set

The ROM font holds 256 glyphs laid out like CP437, so the codes match
the classic box-drawing references. Codes 32-126 are ASCII. The rest:

| Codes | Glyphs |
|---|---|
| 1, 2 | smiling faces (outline, filled) |
| 3, 4, 5, 6 | card suits: heart, diamond, club, spade |
| 7, 9 | bullet, hollow circle |
| 13, 14 | music notes (single, beamed) |
| 16, 17 | right / left pointer (menu cursor) |
| 24, 25, 26, 27 | arrows up, down, right, left; 18 = up/down, 29 = left/right |
| 30, 31 | triangles up, down (scroll indicators) |
| 28 | right angle |
| 174, 175 | `«` `»` |
| 176, 177, 178 | light, medium, dark shade |
| 179-218 | box drawing (single and double lines, all corners, tees and crosses; see below) |
| 219-223 | full block, lower half, left half, right half, upper half |
| 240-243 | `≡` `±` `≥` `≤` |
| 246-250 | `÷` `≈` `°` small bullet, middle dot |
| 253, 254 | `²`, filled square (checkbox) |

Box drawing codes (the ones `ScreenBox` uses in bold):

| | top-left | top-right | bottom-left | bottom-right | horizontal | vertical | cross |
|---|---|---|---|---|---|---|---|
| single | **218** | **191** | **192** | **217** | **196** | **179** | 197 |
| double | **201** | **187** | **200** | **188** | **205** | **186** | 206 |

Single-line tees: 195 `├`, 180 `┤`, 194 `┬`, 193 `┴`. Double-line tees:
204 `╠`, 185 `╣`, 203 `╦`, 202 `╩`. The mixed single/double joins
(181-184, 189, 190, 198, 199, 207-216) are at their CP437 codes too.
Every other code is blank until a program defines it with
`ScreenDefineTile`. Single lines are 2 px thick (matching the font's
stems) on rows/columns 3-4; double lines are 1 px on rows/columns 2 and
5, so adjacent cells join seamlessly.

## Sound and Music (Sound API)

Available as globals; they operate on the program's own audio state
(restored automatically when the program resumes after a launch). The
engine renders 8 score channels plus 8 sound-effect voices, stereo, at
44.1 kHz. The mix is designed to feed the HDMI audio data islands; that
backend is still to come, so audio is only audible under the simulator.
Screen updates are visible on the product board, which scans them out
over HSTX (see `WaitVSync` above).

### Sounds and samples

| Function | Returns |
|---|---|
| `SoundDefine(id, spec)` | `true`, or `nil, err` (id = 0..31) |
| `SoundPreset(name)` | a built-in sound's id (64+) and its spec table, or `nil, err` |
| `SoundPresets()` | the built-in bank: `{ {name=, effect=, id=}, ... }` |
| `SoundLoad(path [, root [, loop_start, loop_end]])` | sound id (32..39), or `nil, err`; `root` is the note recorded (default C4); a loop (frames) sustains a note |
| `SoundPlay(sound [, note [, dur_ms [, vol [, pan]]]])` | voice id (1..8), or `nil, err`; `sound` is an id or a built-in name |
| `SoundStop([voice])` | `true`/`false` (no argument: all one-shots) |
| `SoundStopAll()` | `true` (stops the score too) |
| `SoundVolume(v)` | `true` (master 0..255) |

`SoundDefine` spec: `wave` (`"square"`/`"pulse"`, `"triangle"`, `"saw"`,
`"sine"`, `"noise"`), `duty` (1..15, pulse width in 16ths), `attack`,
`decay`, `release` (ms, 0..255), `sustain` (level 0..255), `volume`
(0..255), `note` (the sound's own pitch, used when a play gives none),
and the pitch/tone effects: `slide` (semitones per second, -3200..3200;
a laser is -300, a jump +180), `vibrato` (depth in cents) with
`vibrato_rate` (Hz), `arp` and `arp2` (semitone steps applied every
`arp_ms` ms; `arp_loop = true` cycles 0, arp, arp2, else the steps are
taken once and held: a coin is `arp = 7, arp_ms = 60`), `cutoff` (a
low-pass tone, 255 open, lower darker). `base = "name"` starts from a
built-in sound and the other fields override it. All fields are
optional.

**The built-in bank.** Like the ROM font, the OS carries sounds a
program plays without defining anything, by name or by id 64 + index
(`audio_presets.c`): instruments, played at the note given (`lead`,
`pulse`, `bass`, `piano`, `organ`, `strings`, `flute`, `brass`, `bell`,
`pluck`, `chime`, `kick`, `snare`, `hihat`, `tom`, `clap`), and effects
that carry their own pitch (`coin`, `jump`, `laser`, `zap`, `explosion`,
`hit`, `hurt`, `powerup`, `blip`, `select`, `error`, `alarm`, `engine`,
`splash`, `bounce`, `teleport`). `SoundPlay("coin")` plays one;
`SoundPreset("laser")` returns its id and spec (change a field and
`SoundDefine` it under your own id, or use `base`).

`note` is a name (`"C4"` = middle C, `"A#3"`, `"Bb3"`, `"C-1"`..`"G9"`) or
a MIDI number 0..127 (default: the sound's own note, else C4). `dur_ms` holds the envelope before
release; omit/0 = one-shot (release after attack+decay). `vol` 0..255,
`pan` -64 (left) .. 63 (right).

`SoundLoad` parses a WAV file (PCM, 8/16-bit, mono/stereo, rate up to
48 kHz) from the SD card into the program's 64 KB sample pool and returns
a sound id (32+n) usable with `SoundPlay` and in scores. Samples play
pitch-shifted from their `root` note (the note recorded, default C4):
a bass sampled at C2 and loaded with `SoundLoad(path, "C2")` plays C2
at its recorded rate and every other note by the difference.

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

Event fields: `at` (ms from the start), `sound` (an id or a built-in
name, required), `note` (default: the sound's own note, else C4), `dur`
(ms; omit/0 = one-shot envelope), `vol` (0..255), `pan` (-64..63).
A channel may instead be a string of packed 10-byte records,
`string.pack("<I4BBI2Bb", at, sound_id, note, dur, vol, pan)` each, which
costs a few bytes of heap per note where a table costs a hundred. The
Sound framework's `Music.Track` writes a score as MML note strings
(`"o4 l8 c d e f g a b > c"`) per channel and compiles them to that.
Events may be written in any order (they are sorted per channel).
`MusicPlay` overrides the score's own `loop` when the second argument is
given.

### Modules

| Function | Returns |
|---|---|
| `ModLoad(path)` | `true`, or `nil, err`: a ProTracker `.mod` (4 channels, 31 samples), one per program |
| `ModPlay([loop])` | `true`, or `nil, err`; loops unless `loop` is false; stops a playing score |
| `ModStop()` / `ModPlaying()` / `ModUnload()` | as for scores |
| `ModPosition()` | `order, row, pattern`, or `nil` |
| `ModInfo()` | `{name, orders, patterns, samples, resident_kb, underruns}` |
| `ModChannels([t])` | a list of four `{sample, note, period, volume, level, active}` (`t` is refilled when given) |
| `ModRows(pattern [, from [, count]])` | rows of a pattern in RAM as `"C-2 05 C40 ..."` strings, or `nil, err` |
| `ModSamples()` | the 31 sample slots as `{name, length, volume, loop}` |
| `SoundSpectrum([t])` | ten band levels 0..255 of everything playing (`t` refilled when given) |
| `SoundSpectrumBands()` | the bands' centre frequencies: 60, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000 Hz |

A module streams from the card: the header, order list and two
patterns are in RAM; samples up to a 48 KB resident budget stay in RAM
(shortest first), the rest keep a 2 KB head so a note starts at once
and stream through 8 KB per-channel rings the OS core refills between
scheduler steps (a loop streams round without a break; one that fits
the ring is kept there). About 82 KB of system heap per loaded module,
which leaves the program itself around 65 KB of Lua heap on the device.
Effects 0-9, A-F and E1/2/5/6/9/A/B/C/D/E are played; the old
15-sample Soundtracker layout loads too; 6/8-channel modules are refused. A module with many long samples can outrun the
card: `ModInfo().underruns` counts the frames a sample was late for.

For visuals, `SoundSpectrum` is a ten-band analyser on the mixed output
(ten resonators fed one frame in four; each band a peak that decays over
about 50 ms), and `ModChannels` gives each module channel's note, sample
and a decaying level; `ModRows` formats pattern rows for a tracker view.
All three refill a table passed in, so a view polling every frame makes
no garbage. `MusicPlaying()` is true from the call to `MusicPlay` until the
score ends or `MusicStop`/`SoundStopAll`; a paused program is silenced
and resumes from its score position.

## The `fs` Module

SD card filesystem, loaded with `local fs = require("fs")`.

### File operations

| Function | Returns |
|---|---|
| `fs.open(path [, mode])` | file object, or `nil, err` |
| `fs.ls([path])` | array of `{name=, size=, dir=}` (default path `/`), or `nil, err` for a folder that does not exist |
| `fs.find(name [, path])` | the real entry name for a case-insensitive match (`fs.find("EDITOR.LUA")` -> `"editor.lua"`), or `nil` |
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
program exits. Note: fs calls run synchronously on the OS core (they
used to be cross-core RPCs); a missing or ejected SD card produces
`nil, err`, never a crash.

## Loading Code from the SD Card

- `loadfile(path)` / `dofile(path)` — replaced with SD-backed versions;
  source files are compiled on the OS core and `.prg` files are loaded as Lua
  5.5 binary chunks.
- A `*.lua` load prefers the matching `*.prg` file when both are present.
  An explicit `*.prg` path is loaded directly.
- `require("name")` — an SD-backed searcher (slot 2) looks for
  `name`, `name.prg`, `name.lua`, `/name`, `/name.prg`, `/name.lua`,
  `/lib/name`, `/lib/name.prg`, `/lib/name.lua` in that order.

The standard Lua 5.5 libraries are available: base, coroutine, table,
string, math, utf8, package.

Relative filesystem paths are resolved against the program's inherited current
working directory. Programs receive that directory as `app.cwd`. An app's
resources are addressed relative to its bundle root, for example
`resources/help.txt` resolves inside `/apps/name.app/resources/`.

## Limits and Semantics

| Limit | Value |
|---|---|
| Program stack depth | 4 programs (including the shell) |
| Lua heap per program | 96 KB (allocation failure raises a Lua memory error, which terminates the program) |
| Script size | 128 KB |
| Timers per program | 8 |
| Pending input events per program | 128 (older events dropped) |
| Sound definitions per program | 32 |
| Scores per program | 8 (shared pool of 1024 events) |
| WAV sample pool per program | 64 KB (8 samples max) |

- `Launch()` from a program pauses it; input arriving while paused goes
  to whichever program is on top when the scheduler drains the queue
  (the topmost interactive one: a running utility has no keyboard).
- Programs cannot call each other's functions; each has its own
  `lua_State`. The only cross-program interaction is launch/exit.
- There is no `os.exit`/`os.time` style API — use `ExitProgram()` and
  `TimeNow()`.
- The shell (`os.lua`) is just another program (pid 0); it lives on the
  card, not in this repo.

## Not Yet Implemented

The HDMI audio data-island output backend (Phase 7): the HSTX video path
is live on the product board (RGB332 scanout, 640x480 at 50 Hz), but audio is
still simulator-only. Programs run fully in the simulator; the sound
engine itself is complete and host-tested meanwhile.

## Card Programs

The programs that run on the device are **not part of this repo**: they
are SPIEdit projects developed alongside it and copied onto the SD card
(the same way any other SPIComputer program is). The OS itself only
provides the runtime and this contract.

In this workspace those projects live in `software/` (`os`, `boot`,
`boot`, `apps` (the launcher), `editor`, `files`, `view`, `chars`, `keys`,
`bench`, `demo`, the utilities `dir`, `copy`, `del`, `ren`, `md`, `rd`,
`touch`, `stat`, `compile`, `help`, `wc`, `grep`, `find`, `tree`, `hexdump`,
`head`, `tail`, `sort`, `calc`, `sysinfo`, `hello`, and the games `breakout`,
`snake`, `spin`). Each builds into its own
`build/` folder; `software/install.sh [folder]` builds them all and
installs the products into a card image (default `software/sdcard/`,
ignored by git) laid out like the card. Copy that folder to a real card,
or point the IDE's Settings > SD card image at it.

The card layout separates the system, installed programs and user data:

- `core/` contains `boot.prg`/`boot.lua` and `os.prg`/`os.lua` (raw
  programs whose projects install to `core`). The shell cannot manipulate
  this directory.
- `apps/` contains applications as `name.app` directories: `app.prg`,
  `app.json`, an optional `icon.*` and `resources/`. `app.json` carries
  `name`, `version`, `description` (one line, set in the IDE's project
  settings), `type` (`application`, `utility` or `game`), `interactive`,
  `video`, `audio`, `entry` and `icon`. The shell's `APPS` picker lists
  apps and games by name and description; RETURN runs the selected one.
- `utils/` contains utilities as `name.util` directories (same contents).
  A utility is a shell command: typing its name runs it with the rest of
  the line as `args`, and its result table is printed.
- `games/` contains games as `name.game` directories. A game replaces the
  shell when launched and the device restarts when it exits.
- `data/` is the user area. `DIR`, `CD`, `MD`, `RD`, `DEL`, `REN`, `MOVE`,
  `COPY`, `TYPE`, `STAT` and `COMPILE` are restricted to this directory.
  Raw programs (`name.prg`/`name.lua`) here run by name.

At power-on firmware boots `core/boot.lua`, preferring `core/boot.prg` when
both exist. A command name resolves to `utils/`, then `apps/`, then
`games/`, then a `.prg`/`.lua` file in `apps/` or `data/`, compiled `.prg`
preferred. The simulator's `sdcard` folder is that
card; it creates `core/`, `apps/`, and `data/` when needed.
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

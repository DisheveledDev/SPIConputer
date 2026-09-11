# SPIComputer OS — Design Notes

This file captures the evolving design of the SPIComputer OS. Treat it as the
source of truth for the architecture. Update it whenever the design changes
or new detail is captured.

## Project Overview

- Bare-metal OS on the Raspberry Pi Pico 2 (RP2350, RISC-V variant).
- All application logic runs in Lua 5.5 (embedded, vendored in `lua/`).
- Video output to HDMI via the RP2350 HSTX peripheral.
- SD card on SPI1 provides the filesystem (FatFs: FAT16/FAT32/exFAT);
  `os.lua` in the card root is the boot script.

## Current Codebase State

| Piece | Status |
|---|---|
| Lua 5.5 core | Built as `lua_core` static lib (`lua/`, `lua.c`/`luac.c` excluded) |
| SD card + FatFs | Vendored in `FatFs_SPI/` (carlk3 no-OS-FatFS-SD-SPI, patched for RP2350/RISC-V). Owned exclusively by core 0 (`fs_core0.c`) |
| Lua `fs` module | `fs_lua.c` — open/read/write/seek/tell/size/close/flush/ls/stat/exists/mkdir/remove/rename/free/ready/readall/writeall, all over the core 1→0 RPC |
| SD-backed loading | `dofile`/`loadfile` globals and `require()` searcher read via RPC (core 1 compiles) |
| RPC transport | `rpc.c` + `system_state.h` — request/response slots, 4 KB staging buffer, semaphore wakeup; FatFs ops only (`rpc.h`) |
| Boot flow | Core 0 mounts SD, services RPCs, feeds watchdog gated on core 1 heartbeat; core 1 boots the shell program (`os.lua`) and runs the scheduler |
| Process model | `program.c` — 4-program stack, per-program Lua state (64 KB heap cap), heap-allocated video state, timers, per-program event rings; `sys_lua.c` exposes TimeNow/Pid/ExitProgram/Launch/TimerCreate/TimerStop/InputPoll/InputControl |
| Shell | `os.lua` — pid 0 program: `dir`, `run <prog> [arg]`, `edit <file>`, `quit` |
| Editor | `editor.lua` — RAM line-buffer editor, mode 1 tile rendering, cursor blink timer, save confirmation; host-tested end-to-end against the mock SD |
| Lua API reference | `lua.md` — the developer contract (entry points, OS/functions/fs/input, limits); keep in sync with the implementation and use as the basis for the future IDE |
| Display | `render.c` (scanline renderer, host-tested golden output) + `screen_lua.c` (ScreenMode/Out/Attr/DefineTile/Palette/Clear/Plot) + `core0/serial_mirror.c` (text protocol to the terminal app, modes 0/1). HSTX/HDMI wiring deferred to Phase 7 |
| Audio | `audio.c` (8-voice stereo synth, score scheduler, WAV sample voices) + `sound_lua.c` (Sound*/Music* API); per-program state like video; host-tested. HDMI data-island feed deferred to Phase 7 |
| Input | `input.c` + `core0/input_hw.c` — 1 kHz matrix scan + joystick poll + RS232 `input=` parsing into a SPSC event queue (`system_state.h`); core 1 drains it in the scheduler loop |
| RS232 terminal app | `../terminal/macos` — sibling folder, not part of the OS. SwiftUI app vendoring the OS protocol parser + ROM font via its `sync-protocol.sh` |
| Desktop simulator | `../simulator/` — sibling folder, not part of the OS. SDL2 app (macOS) running the real OS sources with `sdcard/` as the virtual SD card, an SDL window for video, queued audio, and keyboard/controller input; hardware files replaced by `sim_fs.c`/`sim_main.c` |
| Desktop IDE | `../ide/macos` — sibling folder, not part of the OS. SwiftUI app managing component projects (manifest + Lua/tile/audio/snippet components) and building them into one `.lua`; new projects start with header/main/input/tick components. Editors show line numbers, syntax highlighting and autocomplete for Lua + SPIComputer APIs; a debounced compile check runs the OS's own Lua via `simulator --check` and maps errors back to component lines. Run writes a launcher `os.lua` + the built program into a run-folder SD card and launches the simulator |
| Host tests | `tests/host` — fs bridge + RPC round-trip over mock SD (incl. ejected-card errors), serial-mirror protocol golden vectors, input engine, process model (launch/resume/video restore, tick crash, timers + pause shifting, input deposit, failed launch), audio engine/API, editor |

Build (VS Code Pico extension or CLI):
```bash
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.3.1 PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/RISCV_PICO_2_3_1_0
~/.pico-sdk/cmake/v4.3.4/bin/cmake --build build
```

Desktop simulator (macOS, SDL2). Run these from the folder that contains
`os/` and `simulator/` (the simulator is a sibling of this repo):
```bash
cmake -S simulator -B simulator/build
cmake --build simulator/build
./simulator/build/spicomputer_sim        # first run creates ./sdcard
```

## Hardware Pin Budget (IMPORTANT)

Target chip: **RP2354B** (QFN-80, RP2350B + 2 MB stacked in-package flash).
From the firmware's perspective it is a plain RP2350B (platform `rp2350`,
default 2 MB flash); the stacked flash consumes no GPIOs and no QSPI pins,
so all 48 GPIOs are available. SDK 2.3.1 predates RP2354 board headers, so
a custom `PICO_BOARD` definition will be needed (just
`pico_board_cmake_set` + `PICO_FLASH_SIZE_BYTES = 2 MB`, platform stays
`rp2350`).

HDMI uses the HSTX peripheral on **GPIO 12–19** (all 8 lanes). HSTX is fixed
to GP12–19 in both packages (RP2350A QFN-60 and RP2350B QFN-80).

**Resolved wiring** (see `board_config.h`, the source of truth):

| Consumer | Pins |
|---|---|
| External SPI0 bus (drives/peripherals) | 4 (GP0–3: MISO=0, CS=1, SCK=2, MOSI=3) |
| RS232 dev link (UART1, TTL-level) | 2 (GP4/5) |
| Joystick fire buttons | 2 (GP6/7) |
| SD card (SPI1) | 4 (GP8–11: MISO=8, CS=9, SCK=10, MOSI=11) |
| HDMI (HSTX) | 8 (GP12–19, fixed) |
| Keyboard columns 0–7 (read, pulled up) | 8 (GP20–27, C64 PB0–7 order) |
| Keyboard rows 0–7 (drive, active low) | 8 (GP28–35, C64 PA0–7 order) |
| RESTORE (active low, pulled up) | 1 (GP36) |
| Joystick 1 dirs / Joystick 2 dirs | 8 (GP37–40 / GP43–46) |
| USB (flashing/console) | 0 GPIOs — dedicated package pins |
| **Total** | **47** |

- GP47 is the only spare (XIP_CS1n on RP2350B, avoid unless needed; could
  serve a board LED with the caveat noted).
- USB D+/D- are dedicated RP2350 package pins (not GPIOs), so a USB
  connector for flashing costs nothing from the GPIO budget.
- The external SPI0 bus is pin-defined only (no driver yet); future
  expansion drives share CS lines off GP1.
- **RP2350A (QFN-60, e.g. Pico 2):** 30 GPIOs. Direct wiring does NOT fit
  (47 needed). Options: I2C GPIO expanders (MCP23017 etc.) for keyboard +
  joysticks (2 pins), shift-register input chain (74HC165, 3–4 pins), move
  debug console from UART0 (GP0/1) to USB CDC to free pins, and/or share the
  SPI1 bus between SD and shift registers (separate CS).

## Video Subsystem

Lua issues display commands, e.g. `ScreenMode(0)`. Screen modes fall into two
families: tile/sprite modes and direct pixel mode.

**Tile modes** use a character map of `uchar[w][h]` plus a shadow attribute
map of the same size. Each attribute byte is 8 bits: 1 bit invert +
3 bits colour (choice of 7 colours), 4 bits spare (TBD). The character map
holds tile indexes, **ASCII-aligned where possible** (tile index = character
code, e.g. `screen_data[0][0] = 'Z'`). Tiles are permanently defined as
`uchar[256][8][8]` (mostly static font definitions, fast indexing).
Changing screen mode frees any previous display buffer memory.

**Tile-mode rendering (no pixel framebuffer):** core 0 renders scanlines on
the fly. For output line y: row = y/8, subline = y%8; for each column x,
look up the tile from the char map and fetch that tile's row bytes
(`tiles[tile][subline][0..7]`, or all 8 pixels in one operation), apply
invert/colour from the attribute map, and write into the scanline buffer
for HSTX. Building a whole line at once vs per-pixel is expected to be
similar workload; implement whichever benchmarks better. Only pixel modes
(mode 10) keep an entire framebuffer to stream out.

Mode 0 ignores the colour bits (B&W, invert may still apply); mode 1 uses
the per-character invert + 7-colour attribute.

| Mode | Geometry | Type | Colour |
|---|---|---|---|
| 0 | 40x30 tiles (320x240) | 8x8 tiles + attribute map | 1-bit (B&W, invert attr) |
| 1 | 40x30 tiles (320x240) | 8x8 tiles + attribute map | per-char invert + 7 colours |
| 2 | 80x60 tiles | 8x8 tiles + attribute map | 1-bit |
| 3 | 80x60 tiles | 8x8 tiles + attribute map | per-char invert + 7 colours |
| 10 | 320x240 | direct pixel | 8-bit colour, ~76 KB buffer (streamed out) |

Resolution: **fixed 640x480 output for all modes** (single DVI timing,
25.175 MHz pixel clock, configured once at boot — no mode-switch resync
on the monitor). Modes 0/1 and 10 are logically 320x240 and rendered 2x:
each tile pixel written twice horizontally and each output line sent twice
(trivial in the scanline renderer; aspect ratio is preserved). Modes 2/3
(80x60) render natively. Scanline buffers are always 640 px wide.

Board restriction: the RP2040 dev board supports Mode 0 and Mode 1 only
(B&W 40x30 text over the serial mirror).

**Lua API (implemented, see lua.md for the full reference):**
- `ScreenMode(mode)`
- `ScreenOut(x, y, char [, attr])` — write a tile (+ attribute) to the screen
- `ScreenAttr(x, y, flags)` — attribute-only setter
- `ScreenDefineTile(index, bytes)` — define tile graphics (table of 8
  numbers or an 8-byte string)
- `ScreenPalette(i, r, g, b)` / `ScreenPaletteSet(t)`
- `ScreenClear([char])`
- `ScreenPlot(x, y, colour)` — mode 10 only (provisional, see Q3)

Attribute byte: bit 7 invert, bits 0-2 colour index. Colour index `c`
uses palette entry `c+1` (0 = default white); background is palette
entry 0 (black); invert swaps them. The 4 spare bits stay reserved.

**Implemented (Phase 1, firmware-side scope):**
- `render.c` — platform-neutral scanline renderer (`render_line(y, out)`,
  640 RGB888 per line, 2x scaling for modes 0/1/10), host-tested against
  golden output. Uses the font8x8 ROM font for undefined tiles.
- `screen_lua.c` — the API above; mutations bump `video_state.version`,
  which the serial mirror watches.
- `core0/serial_mirror.c` — streams the text frame protocol to the
  terminal app (resolution/foreground/background headers on change,
  `tile=` lines for redefined tiles, one `data=` frame line per ~3 fps
  refresh), modes 0/1 only, chunked so the main loop stays responsive.
- Mode 10 allocates its 320x240 framebuffer on demand; per the memory
  policy, `Launch` from a mode 10 program fails.

**Deferred to Phase 7 (product board bring-up):**
- HSTX/HDMI output: vendoring an HSTX HDMI library — `pico_scanvideo_dpi`
  (video only) or `pico_hdmi` (video + audio data islands, see the Audio
  Subsystem section; only one library can own HSTX) — plus the fixed
  640x480@60 timing and the DMA pipeline. The current state is
  single-buffered (mid-frame tearing is bounded, see Q18); double-
  buffered maps + vsync swaps land with the HSTX renderer.
- The 4 spare attribute bits remain TBD (see Q4).

**Implementation notes to resolve during planning:**
- HDMI over HSTX; 640x480@60 pixel clock (25.175 MHz) is well within RP2350
  capability. The SDK itself does not ship scanvideo; `pico_scanvideo_dpi`
  from pico-extras is the likely base (verify its RP2350/HSTX support).
- Memory budget: RP2350 has 520 KB RAM. 76 KB pixel buffer + ~16 KB per tile
  set is fine, but per-program video snapshots need a memory policy
  (see process model below).

## Input Subsystem

- **Keyboard:** external 8x8 matrix from a Commodore 64 keyboard connector,
  wired to GPIO: columns 0-7 read on GP20-27 (C64 PB0-7 order, pulled up),
  rows 0-7 driven on GP28-35 (C64 PA0-7 order, active low), RESTORE on
  GP36 (active low, pulled up). Core 0 scans the matrix at 1 kHz from a
  repeating timer IRQ.
- **Controllers:** two DSUB9 joysticks, active-low (switch to GND, pulled
  up): stick 1 dirs GP37-40, stick 2 dirs GP43-46, fires GP6/GP7. Polled
  in the same 1 kHz tick, one event per direction/fire edge.
- **RESTORE:** debounced like the keys and emitted as a modifier key
  event (key 0, `INPUT_MOD_RESTORE` set while held).
- **RS232 keyboard-in:** `input=` lines on the RS232 link feed the same
  event path (the terminal app's keypresses).
- All decoding/debouncing/computation happens in bare metal (core 0);
  Lua only sees events. Debounce is N-samples-of-M (3 consecutive 1 kHz
  samples). Matrix ghosting is accepted (shift+letter = 2 keys is fine,
  3-key chords are not guaranteed).

The unified event struct (`input_event_t`, defined in `system_state.h`,
together with the SPSC queue):

```c
typedef struct {
    uint8_t type;    // KEY | CONTROL1 | CONTROL2
    uint8_t key;     // ASCII key code, or INPUT_KEY_* extended code
    uint8_t mods;    // SHIFT | CTRL | C= | RESTORE bits
    uint8_t pressed; // 1 = down, 0 = up
    uint8_t ctrl;    // which controller (1/2)
    uint8_t dirs;    // UP|DOWN|LEFT|RIGHT|FIRE bitmask
} input_event_t;
```

Events are pushed by core 0 (only the 1 kHz input IRQ) into a
single-producer/single-consumer ring (`g_system_state.input`, 128 deep,
drop-oldest on overflow); core 1 drains it between ticks (Phase 4).

**Key mapping (provisional):** the matrix positions map to ASCII via a
PC-style lookup table in `input.c` (`input_key_table[64]`): letters are
lowercase base / uppercase shifted, digits shift to `!@#$%^&*()`, and
C64 cursor/function keys use the extended codes `INPUT_KEY_*` (128-140,
`system_state.h`). Modifier keys report themselves as code 0 with their
`mods` bit set. The RS232 path sends raw ASCII (`input=` lines); the
terminal app sends arrow keys as ESC [ A/B/C/D sequences which arrive as
three separate KEY events.

Lua entry points (every program must implement):
- `setup()` — called once when the program starts
- `tick()` — called repeatedly; reads and clears pending input events
- `finish()` — called when the program exits

Optional input callbacks (implemented, Phase 3/5 extension):
- `on_keypress(key, shift, ctrl, cbm, restore)` — key-down events only
- `on_control(index, up, down, left, right, fire)` — every joystick
  change with the full state; `index` is the port (0 = joystick 1,
  1 = joystick 2)

The scheduler invokes these from the same drain step that fills the
program's event ring (before the next `tick()`, under the same pcall
error handling as tick). Events remain in the ring either way, so
polling (`InputPoll()`) and callbacks can be mixed; releases and
modifier-key events are only visible by polling.

Input events are deposited by the OS into the program's event state; `tick()`
picks them up and clears them (pull model, callbacks run only at the
scheduler's drain step, never at arbitrary points of execution). Key
events, joystick state (`InputControl(1/2)`: up/down/left/right/fire
bools) and possibly more event types are exposed as values readable
inside `tick()`.

More entry points may be added later. An **RS232 terminal application**
(macOS app, see PLAN.md Phase 2) renders the serial frame stream on a
virtual 40x30 display and sends keyboard input, so programs can be
developed and driven on the dev board before HDMI hardware exists.

## Program / Process Model

- The OS boots into a Lua script that acts as a CLI/shell. It accepts
  keyboard input and executes commands, which may launch another Lua program.
- Every Lua program gets a **pid** stored in its own Lua state (also a
  `pid` global).
- Execution is **free-running tick-driven**: `setup()` runs once, then a
  scheduler loop runs on core 1:

  ```
  loop:
    heartbeat++
    drain input events -> deposit into top program's event ring
    if a timer is due: run due timer callback(s)
    else: tick()
  ```

  `tick()` runs as often and as fast as possible; a due timer delays the
  tick and its callback executes instead. Ticks and timer callbacks never
  overlap (same scheduler loop), so they cannot re-enter each other.
  `tick()` receives no arguments; if a program needs elapsed time (dt) it
  computes and stores it itself from `TimeNow()` (milliseconds since
  boot).
- Launching a program **pauses** the current one (between its ticks): a new
  Lua state is created and ticked; input is routed to the state on top of
  the stack.
- When a program quits, `finish()` runs, its state is popped from the stack
  and:
  1. the previous program's **video state is restored** (its own
     `video_state_t` becomes current again — a pointer swap),
  2. the previous **lua_State resumes ticking** (from its next tick, since
     ticks are atomic).

**Implemented mechanics (Phase 5):**
- Fixed pool of `PROGRAM_MAX` (4) `program_t` slots; allocation failure =
  "can't launch" (clean Lua error).
- Per-program Lua heap cap (64 KB, `capped_alloc` in `program.c`);
  allocation failure raises a Lua memory error, caught by the tick pcall.
  The allocator follows Lua 5.5's contract: for a new block `osize` is a
  *tag*, not a size (only `nsize` is accounted), otherwise the byte
  accounting underflows under churn and every program dies with a bogus
  "not enough memory" (host regression: `test_alloc_churn`).
- Per-program video state is **heap-allocated** (28 KB, outside the Lua
  heap budget) and owned by its program; save/restore on the stack is a
  pointer swap of `g_current_video`. The copy-based snapshot from the
  original plan is unnecessary while states are per-program.
- Exit semantics: `ExitProgram()` sets a flag checked after the current
  tick/timer callback; a throwing `tick()`/timer callback also terminates
  the program. `finish()` runs either way (only if `setup()` completed).
  Errors print to stderr; the parent resumes.
- Input queueing: events drained by the scheduler go to whichever program
  is on top (events that arrive while a program is paused accumulate in
  core 0's queue and go to the current top when drained; overflow drops
  oldest). Per-program event rings are 128 deep.
- `lua_sethook` input-injection machinery is not required for input
  (ticks are atomic and short). No debug hook yet; the heartbeat-gated
  watchdog covers stuck ticks at system level.
- Long-running work inside a tick (e.g. blocking on SD I/O) blocks that
  program only; an explicit `os.wait*()`/yield API may come later if
  programs need to suspend mid-tick.

## Timer Subsystem

Purpose: animations (e.g. swapping between two tiles/sprites every 300 ms,
blinking/inverting the editor cursor) and any delayed work, without blocking
the free-running `tick()`.

- Timers are per-program (callbacks live in the program's Lua state), created
  from Lua with a callback function and a resolution (milliseconds).
- The core 1 scheduler checks timer deadlines each loop iteration using the
  monotonic microsecond clock (`os_time_us()`); due callbacks run in the
  program's state, then the loop continues. Simple deadline array (8 per
  program, array scan).
- While a program is paused (another program on top), its timers pause too.
  On resume, deadlines shift by the pause duration to avoid a burst of
  catch-up callbacks.
- Timers are cleaned up when a program exits.

**Lua API (implemented, Phase 5):**
- `TimerCreate(fn, interval_ms [, oneshot])` -> timer id (callbacks take
  no arguments; 1 ms minimum resolution)
- `TimerStop(id)` -> bool
- Maybe `TimerSetInterval(id, ms)` later if needed.

## Audio Subsystem (planned)

Status: engine, Lua API, per-program state and WAV loading implemented and
host-tested (`audio.c`, `sound_lua.c`, `spicomputer_audio_tests`); the HDMI
data-island output backend is the remaining hardware-gated part (Phase 7).
Product board only — the RP2040 dev board has no HSTX and no allocated
audio pins.

**Output path.** Audio rides the HDMI link as data islands during blanking
(no extra pins). HSTX does not generate HDMI data islands itself, so the
HSTX owner must: `pico_hdmi` (fliperama86, Unlicense) is an HSTX-native
HDMI library with a TERC4 data island queue, audio sample packets, IEC
60958 framing and ACR pacing (44.1/48 kHz supported) — the candidate
backend. `pico_scanvideo_dpi` has no audio, so Phase 7 picks one HSTX
owner. Fallback if HDMI data islands prove unreliable: `pico_audio_i2s`/
`pwm`/`spdif` from pico-extras (costs pins; the budget is full, so only if
HDMI audio fails). The serial link stays video + input only.

**Model.** An 8-channel stereo synth engine, PSG-style first (tone voices
are the core patch type, samples are the later addition):

- **Sounds** are instruments: waveform (square/pulse/triangle/saw/sine/
  noise) + envelope (attack/decay/sustain/release) + pitch effects
  (slide/vibrato/arpeggio). Defined from Lua by id, or predefined.
- **Scores** are per-channel timelines of `(time, sound, note, length,
  volume/pan, effect)` events; they play to the end or loop. Scores are
  defined once (`MusicDefine`) and played by name, so a program can
  pre-build a small jukebox.
- **One-shots**: `SoundPlay` triggers a single voice for sound effects
  without disturbing the score channels.
- **Samples**: `SoundLoad(path)` streams a WAV (8/16-bit PCM, mono/stereo,
  rate ≤ 48 kHz) through the fs RPC into the 64 KB per-program sample pool,
  resampled linearly at playback; a loaded sample doubles as an instrument
  (C4 = original rate) usable with `SoundPlay` and in scores. WAV keeps
  decoding trivial; IMA ADPCM (WAV tag 0x11) remains a possible later 4:1
  size option with a table-based decoder.
- **Song files**: pre-rendered audio is not the plan (a 3 minute tune as
  22 kHz 8-bit mono PCM is ~4 MB — no RAM for it); tunes are score files,
  i.e. a Lua file returning the `MusicDefine` spec, loaded with
  `dofile`/`require`. If tunes ever outgrow the 64 KB Lua heap (large
  table literals), add a line-oriented tracker text format parsed
  incrementally in C. MOD/XM are out: float-heavy decoders and a
  sample-instrument model that does not fit this engine.

**Realtime split.** Lua (core 1) only authors: `*Define` compiles Lua
tables into flat per-program buffers (outside the 64 KB Lua heap) that
core 0 reads; publication is a pointer swap, same as video maps. The
platform-neutral producer (`audio_mix` in `audio.c`) owns the mixer and
the score cursor at 44.1 kHz: per audio block it walks the event
timelines, (re)triggers voices, mixes up to 16 integer voices with
envelopes, and on the product board feeds the HDMI audio packet queue
(underrun = silence + counter; the queue feed lands with the HSTX
backend). Playing/loop state lives with the producer, so playback is
immune to Lua tick jitter; `MusicPlaying()` reads the request/state flags.

**Per-program ownership.** Audio state is per program like video state:
the current program's state is published to core 0 (pointer swap); a
paused program is silenced and resumes from its score position; exiting
frees it.

**Lua API (implemented, see `lua.md` for the full reference):**
- `SoundDefine(id, spec)` / `SoundLoad(path)` -> sound id /
  `SoundPlay(sound [, note [, dur [, vol [, pan]]]])` -> voice id /
  `SoundStop([voice])` / `SoundStopAll()` / `SoundVolume(v)`
- `MusicDefine(name, spec)` / `MusicPlay(name [, loop])` / `MusicStop()` /
  `MusicPlaying()` -> bool

## Core Architecture

RP2350 has two Hazard3 (RISC-V) cores. Planned split:

- **Core 0 (hardware core):** scans the keyboard matrix and joysticks,
  handles all general IO, and prepares/render bytes for HDMI output
  (HSTX + DMA). "The core 0 implementation of all the underlying workings
  will be paramount."
- **Core 1 (Lua core):** runs the Lua engine(s) entirely by itself.

Performance is symmetric: both cores are identical Hazard3 RISC-V cores at
the same clock (150 MHz), each with its own dedicated 16 KB XIP cache
(RP2350 split the cache per core; RP2040 shared one 16 KB cache). The
core split is about responsibility, not speed. Shared-bus contention is the
only asymmetry to be aware of, and hot code (scanline renderer) can be
copied to RAM (`__not_in_flash_func` / PICO_COPY_TO_RAM) on either core.
Both cores always run the same ISA (all-RISC-V or all-ARM, set at boot).

Design principles agreed so far:
- **Strict rule: ALL hardware I/O lives on core 0.** Core 1 never touches
  peripherals; it may block waiting for I/O to complete (RPC).
- Lua may mutate memory that core 0 reads (video state). All shared access
  must be carefully guarded.
- IRQs (SPI, HSTX/DMA, timers) pinned to core 0; core 1 stays IRQ-free for
  deterministic VM behaviour.

**Agreed mechanics:**
- *Video:* don't share a giant pixel buffer where avoidable. For tile modes,
  Lua mutates only the tile map + tile set (small), core 0 renders to
  scanline/framebuffer at frame rate. Swap pointers atomically at vsync
  (double-buffered map or release/acquire swap) instead of locking hot paths.
  Pixel mode (mode 10): double-buffered 76 KB framebuffers (~152 KB, fits).
- *Input:* ~~implemented~~ core 0 decodes and enqueues events into a
  single-producer single-consumer lock-free queue (`input.c`); core 1
  drains the queue between ticks and deposits values for `tick()` to
  read and clear (drain lands with the process model). No cross-core
  Lua calls.
- *SD card / fs:* ~~implemented~~ all FatFs + SPI1 work runs on core 0
  (`fs_core0.c`). Lua `fs` calls on core 1 are blocking RPCs
  (`rpc.c`): core 1 waits (multicore semaphore), core 0 performs the
  operation, replies. FatFs is single-owner/reentrancy safe. `fs_lua.c`
  (formerly `fatfs_lua.c`) sends RPCs and holds handle ids, never
  `FIL*`.
- *Watchdog:* ~~implemented~~ fed by core 0, gated on core 1's heartbeat
  counter advancing (plus a boot grace period and RPC activity), so a
  stuck Lua VM stops the feeds and resets the system.

## First Application: Editor

`editor.lua` (implemented, Phase 6): edits entirely in RAM (array of
lines, no SD block-shifting ever), saves with one `fs.writeall`.
Rendered in mode 1 (40x30 tiles): 29 text lines + an inverted status
line (filename, line/col, dirty flag, save prompt). Cursor blinks via a
500 ms timer.

Keys (host-tested end-to-end in `tests/host/editor_test.c`):

- Cursor keys: extended codes 128-131 from the matrix, and the
  terminal's ESC [ A/B/C/D sequences, both handled.
- Return splits the line; Backspace deletes before the cursor and joins
  lines; Shift+Backspace inserts a space (C64 INST semantics); Forward
  delete deletes at the cursor; Home jumps to the start of the line.
- Ctrl+S saves, Ctrl+Q quits (asks `save? (y/n)` when dirty).

Limitations (accepted for v1): 40-column cursor (no horizontal
scrolling), always-insert mode, saves always end the file with a
newline, no search/replace yet. Files are capped at 128 KB by
`fs.readall` (settles open question 20).

- ~~`fs.readall`/`fs.writeall`~~ implemented (Phase 4, RPC-backed):
  whole-file convenience wrappers, exactly what the editor needs.

## Board Configurations

Development happens on an **RP2040 dev board** (no HSTX/HDMI, fewer pins);
the final product is the **RP2354B** board (custom board header
`boards/rp2354b.h`, `PICO_BOARD=rp2354b` in the product preset). Instead of
an `HDMI_ON` define, a board-type mechanism selects pin assignments and
capabilities at compile time:

| | RP2354B product board | RP2040 dev board |
|---|---|---|
| Platform | rp2350 (RISC-V) | rp2040 (Cortex-M0+) |
| GPIOs | 48 | ~26 usable |
| HDMI (HSTX GP12–19) | yes | no (HSTX absent) |
| SD card (SPI) | SPI1 GP8–11 | SPI1 (e.g. GP10–13) |
| RS232 dev link (115200) | UART GP36/37 | UART GP0/1 or GP4/5 |
| Keyboard matrix (16 pins) | yes | no (too few pins; keyboard via serial) |
| 2x DSUB9 joysticks (10 pins) | yes | no |
| Supported screen modes | all (0/1/2/3/10) | Mode 0 and Mode 1 only (B&W 40x30 text) |
| Stdio | USB | UART |
| Mode 10 double-buffered video | yes (520 KB RAM) | n/a |

Mechanism: use the SDK's own platform defines (`PICO_RP2350` vs
`PICO_RP2040`, set automatically from the selected platform/toolchain) as
the capability switch — no custom capability macros needed for two
platforms. Pin assignments live in `board_config.h` behind
`#if defined(PICO_RP2350)` blocks. **Default build = RP2040 dev
board with RS232 comms** (stdio + program I/O + keyboard over the serial
link); the RP2354B build adds HDMI, key matrix and joysticks. The custom
RP2354B header (`boards/rp2354b.h`, wired in via
`PICO_BOARD_HEADER_DIRS` in the product preset) sets platform rp2350,
2 MB flash and no UART stdio — stdio goes over USB on the product board
(the UART pins carry the protocol stream). If a third board variant ever
appears, add a board-level define on top; platform ifdefs are enough for
now.

Toolchain note: the RP2354B build uses the installed RISC-V toolchain;
the RP2040 build needs an **ARM Cortex-M0+ toolchain** (arm-none-eabi),
which the VS Code Pico extension can install (`pico_set_toolchain`).

## RS232 Development Interface (planned)

A UART link at 115200 bps for headless development (no HDMI):

- **Video out:** the current tile layer (modes 0/1, 40x30) is streamed
  as line-oriented text records. A frame is one `data=` line listing the
  1200 tile values row-major (decimal or 0x hex, cast to uchar) and
  newline-terminated; the client repaints on each complete line.
  `resolution=`, `foreground=` and `background=` are sent at connect and
  whenever they change; the client ignores unchanged values. Custom
  tiles are sent one per line, `tile=<index>,<8 byte values>` (at connect
  and on redefinition); otherwise the client renders with the ROM
  font8x8 set, which is ASCII-aligned (tile index = character code) so
  tile values are meaningful characters. Example frame:

  ```
  resolution=40x30
  foreground=yellow
  background=darkblue
  data=0,0,0,36,38,41,87,0,0,76,98,...
  ```

- **Keyboard in:** the client sends `input=<code>` lines (ASCII code of
  the key). A bare code is synthesised into down+up on the board;
  `input=<code>,1` / `input=<code>,0` give explicit press/release
  (modifiers). Injected into the same input event path as the physical
  key matrix (emulated keyboard).
- **`HDMI_ON` superseded:** replaced by the board-type mechanism above. On
  the RP2040 dev board the serial stream is the only display path; on the
  product board HDMI is active (serial mirror may stay on or off, TBD).
- Owned by core 0 (all I/O on core 0).

Notes:
- Throughput: a full `data=` line is ~4 KB decimal / ~3.6 KB hex; at
  115200 baud that is ~2.3–3.2 fps full refreshes. Slow by design
  (shell output and typing are low-rate; the client holds the last frame
  while the stream updates). A changed-cells delta record is the
  fallback if the editor needs faster feedback.
- Levels: RP2350 UART is 3.3 V TTL. A MAX3232-style transceiver is needed
  for real RS232 ±12 V levels; a TTL USB-serial adaptor works directly.
- Suggested pins: UART1 TX/RX on GP36/GP37 (or UART0 on GP44/45; avoid
  GP47 which is XIP_CS1n on RP2350B).

## Useful External Projects

To evaluate/include where sensible:

- **pico_scanvideo_dpi** (raspberrypi/pico-extras) — DPI/HDMI output over
  HSTX for RP2350: DVI timing, scanline buffers, DMA. Video only (no audio
  data islands). Vendor or FetchContent into the RP2354B build only.
- **pico_hdmi** (fliperama86/pico_hdmi, Unlicense) — HSTX-native HDMI
  output for RP2350 including audio data islands (TERC4 + BCH encoding,
  IEC 60958 sample packets, ACR N/CTS pacing, 44.1/48 kHz, lock-free
  island queue). Candidate HSTX owner once audio is in scope; only one
  library can own HSTX, so pick it or pico_scanvideo_dpi in Phase 7.
- **picoTracker** (xiphonics/picoTracker, BSD-3-Clause) — RP2040 music
  tracker derived from LittleGPTracker: song/chain/phrase/instrument model
  and a fixed-point sample engine. C++ and I2S-based; reference for the
  score model and mixing organisation, not code to lift wholesale.
- **libxm** (Artefact2/libxm, WTFPL) — small XM/MOD/S3M player in C23 with
  no allocations, but float-heavy and Hazard3 has no FPU; worth revisiting
  only if tracker-file playback is ever wanted.
- **font8x8** (dhepper/font8x8) — public-domain 8x8 bitmap fonts (CP437,
  ISO8859), ideal as the ASCII-aligned ROM tile set.
- **littlefs** (littlefs-project/littlefs) — optional later: wear-levelled
  filesystem for the internal 2 MB flash (config, cache) as a complement to
  the SD card. Not needed for v1.
- **Pico-PIO-USB** (sekigon-goccy/Pico-PIO-USB) — PIO-based USB host;
  option if a USB keyboard is ever wanted alongside the C64 matrix.
- **lpeg** (single-file C) — optional pattern-matching lib for the
  editor parsing later.
- **pico-debug** (majbthrd/pico-debug) — dual-core debugging over one probe
  interface; useful dev tooling.
- Reference only (do NOT include): eLua/NodeMCU (task + timer + event model
  inspiration), Lua RTOS. No RTOS (FreeRTOS etc.) in this project — the
  core split and scheduler are intentionally bespoke and minimal.

Also: move the host-side mock FatFs/Lua test harness (currently a scratch
test under /tmp) into the repo as a permanent `tests/host/` CMake target so
bridge logic stays testable without hardware.

## Open Questions

### Video
1. ~~Sprites~~ settled: sprites are just tiles written to the char map; the
   shadow attribute map carries invert + 7-colour selection per cell.
   (Confirm the attribute map also exists at 80x60 in modes 2/3.)
2. ~~Modes 2/3 rendering~~ settled: no pixel buffer; scanline rendering
   from the char map + tile set; fixed 640x480 output for all modes with
   2x scaling for the 320x240 logical modes (0/1/10).
3. **Mode 10 pixel API (provisional):** `ScreenPlot(x, y, colour)` is
   implemented; row blits or a writable buffer object may join it if
   Lua-side performance demands (pixel-at-a-time is slow).
4. **Attribute byte details:** ~~colours~~ settled: bits 0-2 select palette
   entry `c+1` (0 = default white), bit 7 inverts (swaps fg/bg), bg is
   palette entry 0 (black); the 256-entry RGB palette serves mode 10 and
   the attribute colours. The 4 spare bits stay TBD.
5. ~~Tile redefinition~~ settled: `ScreenDefineTile` writes the per-program
   RAM override set, which is part of the program's video state and
   restored on resume (pointer-swap, see process model).

### Input
7. **Key code mapping (provisional):** one unified schema: ASCII codes +
   modifier bits + extended codes 128-140 for C64 cursor/function keys
   (`input_key_table` in `input.c`, PC-style base/shifted pairs). The RS232
   path sends raw ASCII; the terminal sends arrows as ESC [ A/B/C/D
   sequences (three separate KEY events). Revisit after editor experience.
8. ~~Character set~~ settled: font tiles are ASCII-aligned (tile index =
   character code, matching the serial mirror).
9. ~~Joystick type~~ settled: digital-only, 5 lines each (4 dirs + fire),
   active-low with pull-ups.
10. ~~Matrix ghosting~~ accepted: an 8x8 matrix without diodes ghosts on 3+
    keys; shift+letter (2 keys) is fine, chords are not guaranteed.
11. ~~Wiring~~ settled: direct GPIO per the pin budget (see the table
    above; columns GP20-27, rows GP28-35 in C64 order). The RESTORE line
    is wired to GP36 (active low) and reported as the RESTORE modifier
    bit on key-0 events.
12. ~~Event shapes~~ settled: `input_event_t` in `system_state.h`; KEY
    events carry key+mods+pressed, CONTROL events carry ctrl+dirs+pressed.
    Key repeat comes from held keys re-reporting at the OS/editor level
    (TBD in Phase 6); the matrix emits single down/up edges.

### Process model / runtime
13. ~~Program exit semantics~~ settled: `ExitProgram()` (flag checked after
    the current tick/timer callback) or a throwing `tick()`/timer callback
    terminates the program; `finish()` runs either way (when `setup()`
    completed); errors print to stderr and the parent resumes.
14. ~~Stack limits~~ settled: fixed pool of 4 programs, 64 KB Lua heap cap
    each, heap-allocated 28 KB video state per program (outside the Lua
    budget). GC tuning per state remains TBD if tick consistency suffers.
15. ~~Core 1 heartbeat~~ settled: core 0 gates the watchdog feed on the
    heartbeat counter advancing (plus boot grace and RPC activity); a
    stuck tick/VM stops the feeds and resets the system.
16. ~~Timer details~~ settled: 1 ms minimum resolution, callbacks take no
    arguments, `TimerCreate(fn, ms [, oneshot])` / `TimerStop(id)`.
17. **SD program storage layout (provisional):** flat — programs are
    `.lua` files in the SD root, launched by name (`run editor.lua`,
    `edit <file>`). Folders/manifests can come later if needed.
    The shell discovers programs with `dir`.

### Serial / dev interface
18. ~~Stream framing~~ settled: line-oriented text records; the client
    repaints per complete `data=` line, so tearing is bounded to one line
    and corrected by the next frame. A changed-cells delta record is a
    possible later optimisation (see risk register).
19. ~~Audio~~ settled: in scope on the product board — HDMI data-island
    audio (no extra pins); serial stays video/input only. Lua-authored
    sounds and looping multi-channel scores, mixed on core 0; see the
    Audio Subsystem section.

### Editor
20. ~~Editor file size limit~~ settled: 128 KB (the `fs.readall` cap);
    the editor holds the whole file in RAM as an array of lines.

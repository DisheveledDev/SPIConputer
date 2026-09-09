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
| SD card + FatFs | Vendored in `FatFs_SPI/` (carlk3 no-OS-FatFS-SD-SPI, patched for RP2350/RISC-V) |
| Lua `fs` module | `fatfs_lua.c` — open/read/write/seek/tell/size/close/flush/ls/stat/exists/mkdir/remove/rename/free/ready |
| SD-backed loading | `dofile`/`loadfile` globals and `require()` searcher read from the SD card |
| Boot flow | `SPIComputerOS.c` mounts SD, runs `os.lua`, then loops (watchdog fed each iteration) |
| Display | Not yet implemented |
| Input | Not yet implemented |

Build (VS Code Pico extension or CLI):
```bash
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.3.1 PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/RISCV_PICO_2_3_1_0
~/.pico-sdk/cmake/v4.3.4/bin/cmake --build build
```

## Hardware Pin Budget (IMPORTANT)

Target chip: **RP2354B** (QFN-80, RP2350B + 2 MB stacked in-package flash).
From the firmware's perspective it is a plain RP2350B (platform `rp2350`,
default 2 MB flash); the stacked flash consumes no GPIOs and no QSPI pins,
so all 48 GPIOs are available minus board-specific uses (VSYS sense GP24/29,
LED, etc.). SDK 2.3.1 predates RP2354 board headers, so a custom
`PICO_BOARD` definition will be needed (just `pico_board_cmake_set` +
`PICO_FLASH_SIZE_BYTES = 2 MB`, platform stays `rp2350`).

HDMI uses the HSTX peripheral on **GPIO 12–19** (all 8 lanes). HSTX is fixed
to GP12–19 in both packages (RP2350A QFN-60 and RP2350B QFN-80).

**Conflict with current wiring:** the SD card is on SPI1 MISO=GP12, CS=GP13,
which collides with HSTX. The SD card must move to the other SPI1 function
pins: **RX=GP8, CSn=GP9, SCK=GP10, TX=GP11** (GP12–15 are the HSTX copies).

Rough budget:

| Consumer | Pins |
|---|---|
| HDMI (HSTX) | 8 (GP12–19, fixed) |
| SD card (SPI1) | 4 (GP8–11) |
| C64 keyboard matrix 8x8 | 16 |
| 2x DSUB9 joysticks (digital: 4 dir + fire) | 10 |
| RS232 dev interface (UART TX/RX) | 2 (e.g. UART1 GP36/GP37) |
| **Total** | **40** |

- **RP2354B (QFN-80):** 48 GPIOs. Direct wiring fits with ~8 spare (6 if
  RS232 RTS/CTS is added), provided the board exposes all pins.
- **RP2350A (QFN-60, e.g. Pico 2):** 30 GPIOs. Direct wiring does NOT fit
  (38 needed). Options: I2C GPIO expanders (MCP23017 etc.) for keyboard +
  joysticks (2 pins), shift-register input chain (74HC165, 3–4 pins), move
  debug console from UART0 (GP0/1) to USB CDC to free pins, and/or share the
  SPI1 bus between SD and shift registers (separate CS).

## Video Subsystem (planned)

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

**Lua API (planned):**
- `ScreenMode(mode)`
- `ScreenOut(x, y, char)` — write a tile to the screen
- `ScreenDefineTile(index, <byte array>)` — define tile graphics
- Palette API (needed for 8-bit colour): e.g. `ScreenPalette(index, r, g, b)`
  and/or a bulk setter; also global FG/BG colour setters for modes 1/3.
- More screen manipulation functions TBD (capture as they are decided).

**Implementation notes to resolve during planning:**
- HDMI over HSTX; 640x480@60 pixel clock (25.175 MHz) is well within RP2350
  capability. The SDK itself does not ship scanvideo; `pico_scanvideo_dpi`
  from pico-extras is the likely base (verify its RP2350/HSTX support).
- Memory budget: RP2350 has 520 KB RAM. 76 KB pixel buffer + ~16 KB per tile
  set is fine, but per-program video snapshots need a memory policy
  (see process model below).

## Input Subsystem

- **Keyboard:** external 8x8 matrix from a Commodore 64 keyboard connector,
  wired to GPIO (see pin budget). Bare-metal code scans the matrix.
- **Controllers:** two DSUB9 joysticks. Bare-metal code decodes both key and
  joystick activity, then **raises events inside the current (top) Lua state**.
- All decoding/debouncing/computation happens in bare metal; Lua only sees
  events.

Lua entry points (every program must implement):
- `setup()` — called once when the program starts
- `tick()` — called repeatedly; reads and clears pending input events
- `finish()` — called when the program exits

Input events are deposited by the OS into the program's event state; `tick()`
picks them up and clears them (pull model, no callbacks into arbitrary
points of execution). Key events, joystick state (`input_control1`/
`input_control2`: up/down/left/right/fire bools) and possibly more event
types are exposed as values readable inside `tick()`.

The IDE (future) will create projects that enforce this structure; more
entry points may be added later.

## Program / Process Model

- The OS boots into a Lua script that acts as a CLI/shell. It accepts
  keyboard input and executes commands, which may launch another Lua program.
- Every Lua program gets a **pid** stored in its own Lua state.
- Execution is **free-running tick-driven**: `setup()` runs once, then a
  scheduler loop runs on core 1:

  ```
  loop:
    drain input events -> deposit into program event state
    if a timer is due: run due timer callback(s)
    else: tick()
  ```

  `tick()` runs as often and as fast as possible; a due timer delays the
  tick and its callback executes instead. Ticks and timer callbacks never
  overlap (same scheduler loop), so they cannot re-enter each other.
  `tick()` receives no arguments; if a program needs elapsed time (dt) it
  computes and stores it itself from a monotonic time API
  (e.g. `TimeNow()` -> milliseconds since boot), reading the RP2350 timer
  directly on core 1.
- Launching a program **pauses** the current one (between its ticks): a new
  Lua state is created and ticked; input is routed to the state on top of
  the stack.
- When a program quits, `finish()` runs, its state is popped from the stack
  and:
  1. the previous program's **video state is restored** (screen mode, tile
     index map, custom tiles, palette, buffer),
  2. the previous **lua_State resumes ticking** (from its next tick, since
     ticks are atomic).

**Mechanics to resolve during planning:**
- Because ticks are atomic and short, the `lua_sethook` input-injection
  machinery is not required for input. A debug hook may still be useful as
  a watchdog: if a tick runs too long, pause/abort the program and return
  control to the shell.
- Long-running work inside a tick (e.g. blocking on SD I/O) blocks that
  program only; the OS may expose an explicit `os.wait*()`/yield API later
  if programs need to suspend mid-tick.
- Per-process context to snapshot/restore: screen mode, tile index map,
  custom tile definitions (256*64 bytes = 16 KB worst case), palette, and
  which video buffer is active.
- Input queueing: events queued per process while it is paused.

## Timer Subsystem (planned)

Purpose: animations (e.g. swapping between two tiles/sprites every 300 ms,
blinking/inverting the editor cursor) and any delayed work, without blocking
the free-running `tick()`.

- Timers are per-program (callbacks live in the program's Lua state), created
  from Lua with a callback function and a resolution (milliseconds).
- The core 1 scheduler checks timer deadlines each loop iteration using the
  RP2350 microsecond timer (`time_us_64()`); due callbacks run in the
  program's state, then the loop continues. Simple deadline queue (small
  number of timers, array scan or sorted list is fine).
- While a program is paused (another program on top), its timers pause too.
  On resume, deadlines shift by the pause duration to avoid a burst of
  catch-up callbacks.
- Timers are cleaned up when a program exits.

**Lua API (proposed, naming TBD):**
- `TimerCreate(fn, interval_ms [, oneshot])` -> timer id
- `TimerStop(id)`
- Maybe `TimerSetInterval(id, ms)` later if needed.

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
- *Input:* core 0 decodes and enqueues events into a single-producer
  single-consumer lock-free queue; core 1 drains the queue between ticks and
  deposits values for `tick()` to read and clear. No cross-core Lua calls.
- *SD card / fs:* **all FatFs + SPI1 work runs on core 0.** Lua `fs` calls on
  core 1 become blocking RPCs (request queue + response): core 1 waits, core
  0 performs the operation, replies. FatFs remains single-owner/reentrancy
  safe. This changes `fatfs_lua.c` from direct f_* calls to RPC stubs.
- *Watchdog:* fed by core 0.

## First Application: Editor / IDE (planned)

A simple editor/IDE run as the first Lua program: takes keyboard input to
move around a file and insert text, saving back to the SD card.

Requirements noted so far:
- Cursor movement and character insertion/deletion in a text file.
- **Bulk text shifting:** inserting characters in front of others must not
  be O(n) per keystroke on the SD card. Recommended approach: edit entirely
  in RAM (load whole file into a buffer, e.g. gap buffer or array of lines),
  and write the file back on Save. Files are expected to be small (Lua
  scripts, < 128 KB), so in-RAM editing is fast and avoids flash wear.
  Block-shifting on the SD card is only needed if files can exceed RAM.
- `fs` module additions likely needed: `fs.readall(path)` and
  `fs.writeall(path, data)` convenience wrappers (both RPC to core 0),
  possibly `fs.stat`-based size hints for the editor.

## Board Configurations

Development happens on an **RP2040 dev board** (no HSTX/HDMI, fewer pins);
the final product is the **RP2354B** board. Instead of an `HDMI_ON` define,
a board-type mechanism selects pin assignments and capabilities at compile
time:

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
| Mode 10 double-buffered video | yes (520 KB RAM) | n/a |

Mechanism: use the SDK's own platform defines (`PICO_RP2350` vs
`PICO_RP2040`, set automatically from the selected platform/toolchain) as
the capability switch — no custom capability macros needed for two
platforms. Pin assignments live in `hw_config.c` / a small `board_config.h`
behind `#if defined(PICO_RP2350)` blocks. **Default build = RP2040 dev
board with RS232 comms** (stdio + program I/O + keyboard over the serial
link); the RP2354B build adds HDMI, key matrix and joysticks. CMake selects
`PICO_BOARD` (e.g. `pico` for dev; a tiny custom header for the RP2354B
board, or `pico2` + `PICO_FLASH_SIZE_BYTES=2MB` as a stand-in since SDK
2.3.1 predates RP2354). If a third board variant ever appears, add a
board-level define on top; platform ifdefs are enough for now.

Toolchain note: the RP2354B build uses the installed RISC-V toolchain;
the RP2040 build needs an **ARM Cortex-M0+ toolchain** (arm-none-eabi),
which the VS Code Pico extension can install (`pico_set_toolchain`).

## RS232 Development Interface (planned)

A UART link at 115200 bps for headless development (no HDMI):

- **Video out:** the current tile layer (modes 0/1, 40x30) is streamed
  continuously as raw bytes: 40x30 characters followed by a NUL terminator
  (1201 bytes per frame), repeated forever. The host app renders the stream.
  Assumes the font tile set is laid out as ASCII (tile index = character
  code) so raw map bytes are meaningful characters.
- **Keyboard in:** ASCII bytes received on the UART are injected into the
  same input event path as the physical key matrix (emulated keyboard).
- **`HDMI_ON` superseded:** replaced by the board-type mechanism above. On
  the RP2040 dev board the serial stream is the only display path; on the
  product board HDMI is active (serial mirror may stay on or off, TBD).
- Owned by core 0 (all I/O on core 0).

Notes:
- Throughput: 1201 bytes @ 115200 baud ≈ 104 ms/frame ≈ 9.6 fps refresh.
  Modes 2/3 (80x60 = 4801 bytes) would be ≈ 2.4 fps; the serial mirror is
  only practical for 40x30 modes.
- Levels: RP2350 UART is 3.3 V TTL. A MAX3232-style transceiver is needed
  for real RS232 ±12 V levels; a TTL USB-serial adaptor works directly.
- Suggested pins: UART1 TX/RX on GP36/GP37 (or UART0 on GP44/45; avoid
  GP47 which is XIP_CS1n on RP2350B).

## Useful External Projects

To evaluate/include where sensible:

- **pico_scanvideo_dpi** (raspberrypi/pico-extras) — DPI/HDMI output over
  HSTX for RP2350: DVI timing, scanline buffers, DMA. The likely base for
  the HDMI path on the product board. Vendor or FetchContent into the
  RP2354B build only.
- **font8x8** (dhepper/font8x8) — public-domain 8x8 bitmap fonts (CP437,
  ISO8859), ideal as the ASCII-aligned ROM tile set.
- **littlefs** (littlefs-project/littlefs) — optional later: wear-levelled
  filesystem for the internal 2 MB flash (config, cache) as a complement to
  the SD card. Not needed for v1.
- **Pico-PIO-USB** (sekigon-goccy/Pico-PIO-USB) — PIO-based USB host;
  option if a USB keyboard is ever wanted alongside the C64 matrix.
- **lpeg** (single-file C) — optional pattern-matching lib for the
  editor/IDE parsing later.
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
3. **Mode 10 pixel API:** pixel-at-a-time from Lua is slow. Decide shape:
   `ScreenPlot(x, y, colour)`, row blits, or a writable buffer object.
4. **Attribute byte details:** the 4 spare bits (TBD); which 7 colours the
   3 colour bits select (fixed 8-entry palette? C64-like colours?), and
   whether the colour palette for mode 10 (256-entry RGB) is separate.
5. Do programs redefine tiles (ScreenDefineTile) at runtime, and must tile
   definitions therefore be part of the saved/restored video state?

### Input
7. **Key code mapping:** the C64 matrix produces key positions; the serial
   link sends ASCII. Need one unified key event schema (ASCII codes +
   modifiers: SHIFT, CTRL, C=, RESTORE, and how the editor gets cursor
   movement since C64 has no dedicated arrow keys).
8. ~~Character set~~ settled: font tiles are ASCII-aligned (tile index =
   character code, matching the serial mirror).
9. Are joysticks digital-only (5 lines each) or Atari-style with paddle
   (analog) lines?
10. **Matrix ghosting:** an 8x8 matrix without diodes ghosts on 3+ keys.
    Confirm acceptable (shift+letter = 2 keys is fine; chords may not be).
11. Keyboard/joystick wiring approach given the pin budget (direct GPIO,
    I2C expander, or shift registers)? C64 keyboard also has a RESTORE line.
12. Exact event value shapes exposed to `tick()` (key codes, joystick bools,
    key repeat, key-down vs key-up).

### Process model / runtime
13. **Program exit semantics:** how does a program quit? (`finish()` return
    value, explicit `os.exit_program()`, or both?) What happens when `tick()`
    throws — per-tick pcall, error printed where, program terminated?
14. **Stack limits:** max programs paused on the stack, and memory budget
    per Lua state (520 KB total). Lua 5.5 GC is incremental (no long
    pauses), but GC tuning per state may be needed for tick consistency.
15. **Core 1 heartbeat:** core 0 feeds the watchdog; should it require a
    core 1 heartbeat counter to also detect a stuck tick/VM and reboot?
16. Timer details: minimum resolution (1 ms?) and whether timer callbacks
    receive an elapsed-time argument. (dt for `tick()` is settled: no OS
    argument; programs derive it from `TimeNow()` if needed.)
17. **SD program storage layout:** folder per program with a manifest?
    How does the shell discover/launch programs?

### Serial / dev interface
18. **Stream framing:** the raw 40x30+NUL stream needs a resync strategy for
    the host (lock onto NULs); frames can tear while the tile map is being
    updated mid-stream — acceptable, or snapshot/version the frame?
19. **Audio:** confirm HDMI/serial carries no audio (out of scope).

### Editor
20. Editor file size limit / maximum in-RAM buffer size.

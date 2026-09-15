# SPIComputer OS — Design Notes

This file captures the evolving design of the SPIComputer OS. Treat it as the
source of truth for the architecture. Update it whenever the design changes
or new detail is captured.

## Project Overview

- Bare-metal OS on the Raspberry Pi Pico 2 (RP2350, RISC-V variant).
- All application logic runs in Lua 5.5 (embedded, vendored in `lua/`).
- Video output to HDMI via the RP2350 HSTX peripheral.
- SD card on SPI1 provides the filesystem (FatFs: FAT16/FAT32/exFAT);
  the OS boots `core/boot.lua` from the card, preferring `core/boot.prg`.
  The Lua programs themselves are **not** part of this repo: they are SPIEdit
  projects developed alongside it and copied onto the card (see
  "Card programs" below).

## Current Codebase State

| Piece | Status |
|---|---|
| Lua 5.5 core | Built as `lua_core` static lib (`lua/`, `lua.c`/`luac.c` excluded) |
| SD card + FatFs | Vendored in `FatFs_SPI/` (carlk3 no-OS-FatFS-SD-SPI, patched for RP2350/RISC-V). Owned by the OS core (`fs_core0.c`, `core1/`) |
| Lua `fs` module | `fs_lua.c` — open/read/write/seek/tell/size/close/flush/ls/find/stat/exists/mkdir/remove/rename/free/ready/readall/writeall |
| SD-backed loading | `dofile`/`loadfile` globals and `require()` searcher read through the fs layer; source is compiled on the OS core and `.prg` bytecode is loaded directly |
| Filesystem call layer | `rpc.c`/`rpc.h` define the op codes, request/response shapes and the 4 KB staging buffer. Both sides run on the OS core, so `rpc_call` normally dispatches straight into `fs_core0_execute`; the original two-core slot transport survives only for builds that split them (host harness, desktop simulator) |
| Boot flow | Core 0 (video) brings up HSTX and launches core 1 (OS). Core 1 owns stdio, mounts SD, starts the input tick, boots `core/boot.lua` (a timer-driven screen that hands the machine to `core/os.lua` with `Launch(..., replace)`, so boot's Lua state is released) and runs the scheduler, feeding the watchdog |
| Process model | `program.c` — 4-program stack, per-program Lua state (96 KB heap cap, `PROGRAM_HEAP_CAP`), optional heap-allocated audio state, timers, per-program event rings; `Launch(..., replace)` hands the stack over and releases the replaced state; noninteractive utilities keep their isolated Lua state but return `UtilityResult` text to the parent via `UtilityPoll`; `sys_lua.c` exposes TimeNow/Pid/ExitProgram/Launch/Execute/ExecuteString/UtilityResult/UtilityPoll/TimerCreate/TimerStop/InputPoll/InputControl/WaitVSync/Compile (Compile builds a `.prg` from a `.lua` on the card in a scratch `lua_State` on the system heap, same output as the IDE; the shell's `COMPILE` command wraps it) |
| Shell / card programs | **Not in this repo.** `core/boot.prg`, the shell (`core/os.prg`), apps, utilities and games are SPIEdit projects developed outside the OS source tree (this workspace keeps them in `software/`; each builds into its own `build/`, and `software/install.sh` assembles a card image in `software/sdcard/`, git-ignored). Card layout: `core/` (system, raw programs), `apps/` (`name.app`), `utils/` (`name.util`: commands that run once with `args` and return a table via `UtilityResult`), `games/` (`name.game`: launched with `Launch(..., replace)`, so the shell is freed; the device reboots when the last program exits, see `lua_main.c`), `data/` (user files, the only area the shell's file commands touch). The shell resolves a command name to `utils/`, `apps/`, `games/`, then loose programs; `APPS` is a picker of apps and games. It has no exit command: the shell is the OS. The OS only provides the runtime, `lua.md` the contract |
| Lua API reference | `lua.md` — the developer contract (entry points, OS/functions/fs/input, limits); keep in sync with the implementation. The IDE's help panel reads `ide/macos/Sources/SPIIDECore/Resources/help/{lua,os,sdk}.json` (schema in that folder's README): **when an API call, framework function or Lua facility is added or changed, update the matching JSON entry in the same change**; the IDE's tests fail on missing or stale entries |
| Display | `render.c` (scanline renderer, host-tested golden output) + `screen_lua.c` (ScreenMode/Out/Attr/OverlayOut/OverlayAttr/DefineTile/Palette/Clear/Plot) with a base layer plus one overlay. Product-board scanout: `render332.c` (RGB332 fast path, host-tested against `render.c`) + `scanout.c` (HSTX scanline sequencer, host-tested) + `core0/video_hw.c` (TMDS expander, ping/pong DMA, render pump into an 8-line ring) |
| Audio | `audio.c` (8-voice stereo synth, score scheduler, WAV sample voices) + `sound_lua.c` (Sound*/Music* API); per-program state like video; host-tested. HDMI data-island feed deferred to Phase 7 |
| Input | `input.c` + `core1/input_hw.c` — 1 kHz matrix scan + joystick poll into the event queue (`system_state.input`); the scheduler drains it between ticks. Producer and consumer are both on the OS core, so the ring indices are plain words |
| Desktop terminal interface | Removed; display and input development now use the simulator |
| Desktop simulator | `../simulator/` — sibling folder, not part of the OS. SDL2 app (macOS) running the real OS sources with `sdcard/` as the virtual SD card, an SDL window for video, queued audio, and keyboard/controller input; hardware files replaced by `sim_fs.c`/`sim_main.c` |
| Desktop IDE | `../ide/macos` — sibling folder, not part of the OS. SwiftUI app managing component projects (manifest + Lua/tile/audio/snippet components) and building them into `.lua` source plus `.prg` Lua bytecode; new projects start with header/main/input/tick components. Editors show line numbers, syntax highlighting and autocomplete for Lua, the SPIComputer APIs and the project's own functions (with parameter hints); a debounced compile check runs the OS's own Lua via `simulator --check` and maps errors back to component lines, the edited file is syntax-checked two seconds after typing stops and components with errors are flagged in the sidebar, and Return after a block opener auto-inserts the matching `end`. Run writes both outputs into a run-folder SD card and boots the `.prg` directly with `simulator --boot` |
| Host tests | `tests/host` — fs bridge (both the direct firmware path and the two-core slot transport) over mock SD (incl. ejected-card errors), scanout sequencer + RGB332 renderer, base+overlay renderer, input engine, process model (retire queue, WaitVSync), audio engine/API, editor |

Build (VS Code Pico extension or CLI):
```bash
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.3.1 PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/RISCV_PICO_2_3_1_0
~/.pico-sdk/cmake/v4.3.4/bin/cmake --build build
```

Desktop simulator (macOS, SDL2). Run these from the folder that contains
`system/` and `simulator/` (the simulator is a sibling of this repo):
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
`uchar[256][8]` (8 row bytes; bit 0 is the leftmost pixel, matching the
ROM font), which also keeps them mostly static and fast to index.
Changing screen mode frees any previous display buffer memory.

**Core split (video core / OS core).** Core 0 does one thing: the HSTX
scanout. `core0/main.c` sets the clock, brings the display up (`video_hw_init`),
launches core 1, then alternately pumps the render (`video_hw_poll`)
and idles in WFI while the video DMA IRQ feeds the display from core
0's screen slots (see video.h). Rendering runs in that loop, never in an
ISR: the DMA
completion must be able to preempt it, or the 8-word HSTX FIFO starves
while a row is drawn. Core 1
(`core1/lua_main.c`) runs everything else: stdio, FatFs + the SD SPI,
the 1 kHz input tick, the watchdog and the Lua scheduler. Consequences
worth remembering:

- The cross-core objects are `g_system_state.video_frame_count`
  (written by core 0 at each vertical blank, read by core 1) and the
  display op queue (`video.h`): core 1 appends small ops (`ScreenOut`,
  `ScreenPalette`, ...) and core 0 drains and applies them at each frame
  boundary. No RPC, no locks.
- The display state lives on **core 0** (one `video_state_t` slot per
  program, ~10 KB each). Core 1 never reads or writes it, so there are
  no frame snapshots and no cross-core copies; a full queue blocks the
  drawing call until core 0's next drain, which paces drawing to the
  frame rate.
- IRQ affinity: each IRQ is enabled on the core that should take it.
  The video DMA IRQ (DMA_IRQ_2) on core 0; the SD
  SPI's DMA_IRQ_0, the input timer and USB on core 1. The SDK keeps one
  handler table but per-core enables, so no IRQ may be enabled on both.
- The alarm pool/timer IRQ belongs to whichever core first uses it, so
  core 0 must not call `sleep_ms()`, timers or `delay` functions - it
  uses `busy_wait_us()` for the one regulator settle at boot. If it
  claimed the pool, the input tick would be delivered to the video core.
- stdio is core 1's: core 0 must not print. Anything core 0 wants
  reported (the HSTX clock, scanline, underruns) is read through
  `video_hw.h` getters and printed by core 1.
- Stacks: core 1's SDK stack would live in SCRATCH_X (4 KB), so it is
  given a 16 KB stack in main SRAM via
  `multicore_launch_core1_with_stack()` (`PICO_CORE1_STACK_SIZE=0`);
  core 0's `.stack` in SCRATCH_Y is 4 KB (`PICO_STACK_SIZE`).
- Watchdog: fed from core 1's loop only while the loop is stepping *and*
  the display is producing frames (a 250 ms stall limit, 5 s boot
  grace). A stuck VM, a deadlock or a dead scanout all reset the board.
  The boot phase runs with a longer 8 s period because the SD mount and
  the first program load can outlast the normal one.

**Tile-mode rendering (no pixel framebuffer):** core 0 renders scanlines on
the fly. For output line y: row = y/8, subline = y%8; for each column x,
look up the tile from the char map and fetch that tile's row byte
(`tiles[tile][subline]`, eight pixels per byte), apply
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
| 10 | 320x240 | direct pixel | 8-bit colour, core 0's shared 76 KB buffer |

The 80x60 modes 2/3 are retired for now (`ScreenMode` refuses them).

Resolution: **fixed 640x480 output for all modes** (single DVI timing,
25.2 MHz pixel clock, configured once at boot — no mode-switch resync
on the monitor). Modes 0/1 and 10 are logically 320x240 and rendered 2x:
each tile pixel written twice horizontally and each output line sent twice
(trivial in the scanline renderer; aspect ratio is preserved). Scanline
buffers are always 640 px wide. The default refresh is **60 Hz** (the
VESA 800x525-line timing; `-DSPICOMPUTER_REFRESH_HZ=50` selects a
non-standard 800x630 mode that gives core 0 a 4.8 ms vblank but that
some monitors refuse to lock to; the display produced no output while
it was the default).

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
- `render.c` — platform-neutral scanline renderer (`render_line(ly, out)`,
  640 RGB888 per logical row, 2x scaling), host-tested against golden
  output. Uses the ROM font for undefined tiles.
- ROM font: `font8x8_rom.h` (256 glyphs, generated by `tools/mkfont.py`
  from `font8x8_basic.h` plus pixel art in the script; regenerate with
  `python3 tools/mkfont.py > font8x8_rom.h` from `system/`). Laid out
  like CP437: box drawing and blocks at 0xB0-0xDF, arrows/pointers/suits
  in 0x01-0x1F, a few symbols at 0xF0-0xFE, the rest blank. The
  box-drawing glyphs are built from arm rules (single = 2 px on
  rows/cols 3-4, double = 1 px on rows/cols 2 and 5) so cells join;
  `tools/mkfont.py --show` prints them for review. `screen_lua.c`'s
  `ScreenBox`/`ScreenFill` (and the Overlay pair) draw frames and
  rectangles with these codes, one queued op per cell.
- `screen_lua.c` — the API above; each call queues an op for core 0.
  Text modes provide a base layer plus one composited overlay.
- Block ops (`video.h` `VIDEO_OP_RECT/COPY/SCROLL/BOX/TEXT`): one op per
  rectangle, applied by core 0 in `video.c` (`ScreenFill/FillAttr/Copy/
  Move/Scroll/Box/Write/WriteAttr` and the Overlay twins). `TEXT` carries
  its bytes through a two-slot staging buffer (`video_staging_acquire`,
  `video_op_put_staged`): the producer reuses a slot only after the drain
  has applied the op that referenced it, which is why the drain applies
  an op *before* advancing the head. Frameworks (the IDE's SDKs) are
  Lua over these calls; the input callbacks are looked up by name per
  event so a framework can install its dispatcher from `setup()`.
- Mode 10 uses core 0's shared 320x240 pixel buffer; entering the mode
  attaches and clears it, and per the memory policy `Launch` from a
  mode 10 program fails.

**Implemented (Phase 7, video):**
- CPU clock: `core0/main.c` overclocks `clk_sys` to 252 MHz (the
  `board_config.h` default; 2x the 126 MHz baseline; core voltage
  1.25 V) before stdio comes up, because a scanline
  must never be missed and the display core still shares the clock (and
  the bus) with the OS core. The clock must let `clk_hstx` be
  `clk_sys / 1..3 = 126 MHz`
  to keep the exact 25.2 MHz pixel clock (the HSTX divider only divides
  by 1..3), so 252 (/2) and 378 (/3, 1.30 V) are the candidates; a
  378 MHz build gave no HDMI sync at all on the pico2 prototype, so it
  stays opt-in until the display is proven at 252 and the clock can be
  raised as the one variable. `SPICOMPUTER_SYS_CLOCK_KHZ` overrides the target; 400 MHz is
  achievable but then `clk_hstx` is 133.3 MHz, i.e. a ~26.7 MHz pixel
  clock and ~63.5 Hz refresh (off DVI spec but usually still locked),
  while 252 MHz (2x) stays exact. `clk_hstx` cannot be moved to
  `pll_usb` to decouple it: USB stdio pins that PLL to 48/96/144 MHz.
  The QSPI flash is clocked from `clk_sys / PICO_FLASH_SPI_CLKDIV` (75 MHz
  at boot), so `flash_scale_clock()` (SRAM-resident, before the jump)
  scales the QMI divider and RX sampling delay to keep the flash clock and
  the sample point in the data eye unchanged. If the requested clock is
  not exactly attainable the firmware falls back to 126 MHz.
- HSTX video: TMDS expansion for RGB332, fixed 640x480 with negative
  sync polarity (60 Hz vertical timing by default); `clk_hstx` is
  divided down from `clk_sys` to 126 MHz so
  the pixel clock is 25.2 MHz at any supported CPU clock (the boot log
  prints the divisor and pixel clock, and warns if no divisor is close).
- Scanout pipeline: `scanout.c` sequences the ping/pong DMA (43 vblank
  lines, a command list plus a pixel line per active line) into an 8-row
  ring; `core0/video_hw.c` renders ahead from core 0's main loop
  (`video_hw_poll`), so the DMA IRQ never renders and can always preempt
  the renderer: a render ISR would block the post long enough to starve
  the 8-word HSTX FIFO.
- Ring protocol (the failure mode this is built around): the ring holds
  *rows of the current frame* in `row % SCANOUT_RING_LINES`; the producer
  publishes rows only while `rows_published - rows_consumed <
  SCANOUT_RING_AHEAD` (= RING - 2), because a buffer posted at completion
  IRQ S is transferred between IRQs S+1 and S+2, so the two most recently
  posted rows are still in the DMA pipeline and must not be overwritten.
  At the end of the active region both counters reset and
  `scanout_frame_begin()` refreshes the mode, so the vblank prefetch is
  bounded (RING - 2 rows) and uses the geometry of the frame it feeds.
  `scanout.c`/`scanout_frame_begin` run from SRAM (an XIP miss in the
  sequencer would miss a line); the host tests in
  `tests/host/scanout_test.c` drive this state machine and check the
  *pixel data* that reaches the scanout plus the in-flight invariant, not
  just the post order.
  `video_hw_underruns()` counts scanlines that had to repeat (printed once
  per second from the main loop).
- Frame signal: `g_system_state.video_frame_count` is written at each
  vertical blank; `WaitVSync([ms])` in `sys_lua.c` reports frames elapsed
  since the program's previous call. HSTX audio data islands remain the
  only deferred Phase 7 piece.
- Program screens persist across launches: each program owns a core 0
  slot (its pool index), selected by a queued op on launch and on exit,
  so returning to the shell restores its screen with no copy.
- Attribute bit 6 is reserved for transparent overlay cells.
- Chequerboard test pattern: `video_hw_set_test_pattern(true)` makes
  core 0 draw a fixed 8x6 board of saturated colours (40x40 logical px
  squares, white 1 px border) instead of the program slots; the flag is
  latched at the frame boundary. Core 1 turns it on when the card fails
  to mount or the boot program cannot start (a missing card and a dead
  display then look different), and the CMake option
  `-DSPICOMPUTER_CHEQUERBOARD=ON` forces it: no SD access, no Lua, so
  what the monitor shows is the HDMI path alone. (The older
  `SPICOMPUTER_VIDEO_TEST_PATTERN` build keeps the 640x480 band pattern
  with the underrun bar.)
- Bring-up log: the bench has no working USB console, so core 1 also
  appends its boot report and a video status line per
  `SPICOMPUTER_LOG_PERIOD_MS` to `spilog.txt` in the card root (one
  open/append/close per period, so nothing flushed is lost to a reset).
  Each boot logs the POWMAN reset reason (power-on, brown-out, RUN pin,
  watchdog, ...) plus the previous run's uptime and loop phase, kept in
  watchdog scratch 0-2. A frame-counter stall is flushed immediately,
  ahead of the watchdog reset. Test-pattern builds do no SD work and so
  log nothing. Two performance lines accompany each status line:
  `render:` (core 0's row renderer: rows, average and worst row time
  against the 63.5 us row budget, from `video_hw_render_stats`) and
  `lua:` (time inside Lua callbacks on core 1: calls, average/min/max,
  percent busy, and the top program's heap use, from
  `program_lua_stats`). Take these before and after any change to the
  Lua build or to core 0's render loop.
- Lua benchmark: `software/bench` (SPIEdit project, app `bench`) runs a
  fixed-size test per tick (integer/float loops, calls, table array and
  hash churn, strings, OS API calls, `ScreenOut`, a full GC) and writes
  `data/bench.txt` (build line, then `test,ms,ops_per_ms`). Results are
  comparable across firmware builds; a `!` suffix marks a test that hit
  the heap cap. The simulator is single-threaded, so a tick that
  queues more than 1024 display ops hangs it (the board just blocks
  until the next frame); the benchmark stays under that.
- `scanout_frame_begin` must set `rows_total` to `VIDEO_FB_ROWS` (240
  logical pixel rows). Setting it to `VIDEO_ROWS` (30 tile rows) made
  the sequencer rebase the frame every 60 output lines: most of the
  picture was underruns (frozen on the last row), the op drain ran
  eight times per frame and the frame-step diagnostic skewed.

**Implementation notes to resolve during planning:**
- HDMI over HSTX; fixed 640x480 with a 25.2 MHz pixel clock (VESA's
  25.175 MHz spec), 60 Hz vertical timing by default. The SDK itself
  does not ship scanvideo; `pico_scanvideo_dpi`
  from pico-extras is the likely base (verify its RP2350/HSTX support).
- Memory budget: RP2350 has 520 KB RAM. 76 KB pixel buffer + 2 KB per tile
  set is fine, but per-program video snapshots need a memory policy
  (see process model below).

## Input Subsystem

- **Keyboard:** external 8x8 matrix from a Commodore 64 keyboard connector,
  wired to GPIO: columns 0-7 read on GP20-27 (C64 PB0-7 order, pulled up),
  rows 0-7 driven on GP28-35 (C64 PA0-7 order, active low), RESTORE on
  GP36 (active low, pulled up). The OS core scans the matrix at 1 kHz
  from a repeating timer IRQ.
- **Controllers:** two DSUB9 joysticks, active-low (switch to GND, pulled
  up): stick 1 dirs GP37-40, stick 2 dirs GP43-46, fires GP6/GP7. Polled
  in the same 1 kHz tick, one event per direction/fire edge.
- **RESTORE:** debounced like the keys and emitted as a modifier key
  event (key 0, `INPUT_MOD_RESTORE` set while held).
- **RS232 keyboard-in:** `input=` lines on the RS232 link feed the same
  event path (the terminal app's keypresses).
- All decoding/debouncing/computation happens in bare metal (the OS
  core); Lua only sees events. Debounce is N-samples-of-M (3 consecutive 1 kHz
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

Events are pushed by the 1 kHz input IRQ (the only producer) into a
single-producer/single-consumer ring (`g_system_state.input`, 128 deep,
drop-oldest on overflow); the scheduler drains it between ticks
(Phase 4). Producer and consumer are both on the OS core now, so the
indices are plain words rather than atomics.

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

- The OS boots the card's `boot.lua`, which typically launches a shell
  (`os.lua`) in the foreground and exits when it exits. The shell accepts
  keyboard input and executes commands, which may launch another Lua
  program. All of those programs live on the card, not in this repo.
- Every Lua program gets a **pid** stored in its own Lua state (also a
  `pid` global).
- Execution is **free-running tick-driven**: `setup()` runs once, then a
  scheduler loop runs on core 1:

  ```
  loop:
    drain input events -> deposit into top program's event ring
    if a timer is due: run due timer callback(s)
    else: tick()
    (feed the watchdog while this loop is stepping and video is alive)
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
- Per-program Lua heap cap (96 KB, `capped_alloc` in `program.c`; it was
  64 KB until the shell's APPS picker needed ~45 KB on the device for the
  shell alone. The cap is a limit, not a reservation: four programs at
  the cap would not fit the ~290 KB heap, but the shell plus one app do);
  allocation failure raises a Lua memory error, caught by the tick pcall.
  The allocator follows Lua 5.5's contract: for a new block `osize` is a
  *tag*, not a size (only `nsize` is accounted), otherwise the byte
  accounting underflows under churn and every program dies with a bogus
  "not enough memory" (host regression: `test_alloc_churn`).
- Per-program **screen slots** live on core 0 (one ~10 KB state per
  program; no heap use on core 1). The program's only display memory is
  the small op queue (`video.h`), drained by core 0 at frame boundaries;
  the Lua API blocks only when that queue is full. The audio state
  remains heap-allocated per program (outside the Lua heap budget).
- Exit semantics: `ExitProgram()` sets a flag checked after the current
  tick/timer callback; a throwing `tick()`/timer callback also terminates
  the program. `finish()` runs either way (only if `setup()` completed).
  Errors print to stderr; the parent resumes.
- Handover: `Launch(path, arg, true)` replaces the caller instead of
  stacking on it. The caller leaves the stack (its parent becomes the
  new program's parent) and its Lua state, timers and audio are released
  once the launching call returns, because the caller is still inside
  its own Lua frame at that point. `core/boot.lua` uses this to hand the
  machine to the shell without staying resident, and runs on timers
  rather than `tick()` so the scheduler idles between screen updates.
- Input queueing: events drained by the scheduler go to whichever program
  is on top (events that arrive while a program is paused accumulate in
  core 0's queue and go to the current top when drained; overflow drops
  oldest). Per-program event rings are 128 deep.
- `lua_sethook` input-injection machinery is not required for input
  (ticks are atomic and short). No debug hook yet; the watchdog (fed from
  the scheduler loop, gated on the display producing frames) covers stuck
  ticks at system level.
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
  rate ≤ 48 kHz) through the fs layer into the 64 KB per-program sample pool,
  resampled linearly at playback; a loaded sample doubles as an instrument
  (C4 = original rate) usable with `SoundPlay` and in scores. WAV keeps
  decoding trivial; IMA ADPCM (WAV tag 0x11) remains a possible later 4:1
  size option with a table-based decoder.
- **Song files**: pre-rendered audio is not the plan (a 3 minute tune as
  22 kHz 8-bit mono PCM is ~4 MB — no RAM for it); tunes are score files,
  i.e. a Lua file returning the `MusicDefine` spec, loaded with
  `dofile`/`require`. If tunes ever outgrow the Lua heap cap (large
  table literals), add a line-oriented tracker text format parsed
  incrementally in C. MOD/XM are out: float-heavy decoders and a
  sample-instrument model that does not fit this engine.

**Realtime split.** Lua (core 1) only authors: `*Define` compiles Lua
tables into flat per-program buffers (outside the capped Lua heap) that
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

RP2350 has two Hazard3 (RISC-V) cores. The split is by *real-time
criticality*, not by hardware vs software:

- **Core 0 (video core):** the HSTX scanout and nothing else. It sets the
  clock, brings up the display, launches core 1, and then runs the render
  pump around WFI while the video DMA IRQ keeps the display fed. No
  stdio, no SD, no input, no watchdog, no alarm pool - nothing that could
  delay a scanline or thrash the XIP cache under it.
- **Core 1 (OS core):** everything else - stdio, FatFs and the SD SPI,
  the 1 kHz input tick, the watchdog, and the Lua scheduler. Filesystem
  calls are direct function calls now (no cross-core RPC), and input is
  produced and consumed on the same core, so the only shared object in
  the system is the video frame counter.

This replaced the earlier "core 0 owns all I/O, core 1 runs Lua" split:
a long SD read on core 0 shared the core, the XIP cache and the bus with
the scanout, and every cross-core service (SD, input, heartbeat) needed a
queue or a semaphore. Giving video a core of its own removes those
liabilities; the cost is the affinity rules listed under "Core split"
above (IRQ, timer-pool and stdio ownership).

Performance is symmetric: both cores are identical Hazard3 RISC-V cores at
the same clock (252 MHz, see the Phase 7 notes). They share one 16 KB XIP
cache, and so does the DMA. The core split is about responsibility, not
speed. Both cores always run the same ISA (all-RISC-V or all-ARM, set at
boot).

**Core 0's scanout path never touches flash.** Everything it executes
or reads (and everything the video DMA reads) lives in SRAM: the DMA
IRQ and sequencer, the render pump and its idle loop, `render332.c`, the
op drain in `video.c`, the HSTX command lists, the font copy and the
tables they use (`__not_in_flash_func` / `__not_in_flash`). Those files
are built with `-fno-jump-tables` so a `switch` cannot put its table in
flash, and the drain uses its own fill loop rather than the flash
`memset`. The rule exists because a Lua program on core 1 evicts
whatever core 0 had cached. With the command lists in flash, the DMA
stalled on QSPI refills, the 8-word HSTX FIFO ran dry and the monitor
lost sync as soon as Lua drew anything; the chequerboard (with an idle
core 1) looked perfect. After changing core 0 code, check the ELF: no
function on that path may call or load from a `0x10xxxxxx` address.

Design principles:
- **Real-time isolation:** core 0 runs only the scanout. Anything added
  there must justify itself against a missed scanline.
- **Single-owner I/O:** each peripheral, IRQ and timer belongs to exactly
  one core, and is initialised on that core (see the affinity rules).
- Lua may mutate memory that core 0 reads (the current video state). The
  retire queue and the frame counter are the only synchronisation: video
  states are freed two frame boundaries after their program exits, so a
  scanline in flight can never touch freed memory.

**Agreed mechanics:**
- *Video:* don't share a giant pixel buffer where avoidable. For tile modes,
  Lua mutates only the tile map + tile set (small), core 0 renders to
  scanline/framebuffer at frame rate. Swap pointers atomically at vsync
  (double-buffered map or release/acquire swap) instead of locking hot paths.
  Pixel mode (mode 10): double-buffered 76 KB framebuffers (~152 KB, fits).
- *Input:* ~~implemented~~ the OS core's 1 kHz timer IRQ decodes and
  enqueues events into a single-producer single-consumer ring
  (`input.c`); the scheduler drains it between ticks and deposits values
  for `tick()` to read and clear. Same core both sides, so the ring
  indices are plain words. No cross-core Lua calls.
- *SD card / fs:* ~~implemented~~ all FatFs + SPI1 work runs on the OS
  core (`fs_core0.c`). `fs_lua.c` (formerly `fatfs_lua.c`) holds handle
  ids, never `FIL*`, and reaches FatFs either directly
  (`fs_core0_execute`, the firmware path) or through the retained
  two-core slot transport (`rpc.c`, used by the host harness and the
  simulator). FatFs is single-owner/reentrancy safe.
- *Watchdog:* ~~implemented~~ fed by the OS core's scheduler loop while
  the loop is stepping *and* core 0 is producing frames; a stuck Lua VM,
  a deadlock or a frozen display resets the system. The boot phase (SD
  mount, first program load) runs with a longer period.

## First Application: Editor

`software/editor` (an *application* project, installed as
`apps/editor.app`): edits entirely in RAM (array of lines, no SD
block-shifting ever), saves with one `fs.writeall`. Written on the
Screen/Overlay/Text/Timer frameworks, mode 1 (40x30 tiles): row 0 is a
menu bar (FILE / EDIT / OPTIONS / HELP), rows 1-28 the text, row 29 an
inverted status line (file name, line/col, dirty flag). The base layer
holds only the text; the open drop-down menu and the dialogs (go to
line, file info, keyboard help, unsaved-changes prompt) are
`Overlay.Window`/`Overlay.Dialog` on the overlay, so dismissing them is
one `Overlay.Clear()` with no text redraw. The cursor is the invert
attribute on its cell, blinked by a `Timer.Every(500)` that is paused
while the overlay is showing. Input arrives through `on_keypress`.

Keys (host-tested end-to-end in `tests/host/editor_test.c`, which needs
the built `software/editor/build/editor.lua` on its command line):

- Cursor keys: extended codes 128-131; Home jumps to the start of the
  line. F1-F4 (and Ctrl+H/F/E/O) open help and the three menus; arrows
  move within and between menus, Return chooses, Esc closes.
- Return splits the line; Backspace deletes before the cursor and joins
  lines; Shift+Backspace inserts a space (C64 INST semantics); Forward
  delete deletes at the cursor.
- Ctrl+S saves, Ctrl+Q quits (an overlay dialog asks Y/N/Esc when dirty).

Shell note: a launched program's `setup()` runs inside the shell's
`Execute` call, so the shell must not repaint at the end of that tick
(it checks `needs_repaint`) or it paints over the new program's screen.

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

| | RP2354B product board | Pico 2 prototype | RP2040 dev board |
|---|---|---|---|
| Platform | rp2350 (RISC-V) | rp2350 (ARM) | rp2040 (Cortex-M0+) |
| GPIOs | 48 | 26 | ~26 usable |
| HDMI (HSTX GP12–19) | yes | yes | no (HSTX absent) |
| SD card (SPI) | SPI1 GP8–11 | SPI1 GP8–11 | SPI1 GP10–13 |
| C64 keyboard/joysticks | yes | no | no |
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

## Development Interface

Development and UI testing use the desktop simulator, which renders the
complete layered video state locally and provides keyboard and controller
input without a serial transport.

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
14. ~~Stack limits~~ settled: fixed pool of 4 programs, 96 KB Lua heap cap
    each, heap-allocated 28 KB video state per program (outside the Lua
    budget). GC tuning per state remains TBD if tick consistency suffers.
15. ~~Watchdog liveness~~ settled: the OS core feeds the watchdog while
    its scheduler loop is stepping and core 0 is still producing frames
    (boot grace and a longer boot period); a stuck tick/VM or a frozen
    display resets the system.
16. ~~Timer details~~ settled: 1 ms minimum resolution, callbacks take no
    arguments, `TimerCreate(fn, ms [, oneshot])` / `TimerStop(id)`.
17. **SD program storage layout:** `core/` (boot and the shell),
    `apps/` (`name.app`), `utils/` (`name.util` commands), `games/`
    (`name.game`), `data/` (the writable user area). A command name
    resolves to `utils/`, `apps/`, `games/`, then loose programs in
    `apps/`/`data/`; file manipulation commands are restricted to
    `data/`. The programs themselves are external projects, not part of
    the OS source tree.

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

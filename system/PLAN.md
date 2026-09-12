# SPIComputer OS — Implementation Plan

Companion to `AGENTS.md` (design notes). This document breaks the design
into ordered phases with concrete tasks, acceptance criteria, and the
reasoning behind the main implementation decisions.

## Guiding Implementation Style

- **C11 only for all OS code.** No C++ in our code (the toolchain links
  with g++ only because the SDK enables CXX for `new_delete`; we stay C).
- **No RTOS, no dynamic allocation on core 0.** Core 0 is deterministic:
  static buffers, fixed tables, no malloc in hot paths. All heap activity
  lives on core 1 inside Lua's allocator.
- **Shared state is explicit and small.** One `system_state_t` struct
  describes everything shared across cores: video state (mode, map
  pointers, attributes, palette), input event queue, RPC queues. Every
  field has a documented access rule (owner, or guarded by which primitive).
- **Lock-free where possible, brief spinlocks otherwise.** Video map
  updates are pointer swaps (double-buffered) so no lock is ever held on
  the render path. Queues are single-producer/single-consumer with
  release/acquire atomics. The SDK's multicore-safe mutex/semaphore
  primitives are used only for rare, slow operations (mode changes, RPC
  completion).
- **Lua integration follows the existing `fs_lua.c` style:** one C file
  per module, registry metatables for objects, light validation, clear
  nil+error return convention. Never call Lua from core 0.
- **Everything compiles for two targets:** the RP2040 dev board (default,
  RS232-only) and the RP2354B product board. Platform `#ifdef`s switch pins
  and capabilities; unsupported features compile out entirely.
- **Host-testable.** Anything not touching registers (video state
  machine, renderer, Lua bridge, RPC protocol) gets a mock header and
  a host test in `tests/host/`, so bridge and renderer logic stays
  verifiable without hardware.

---

## Phase 0 — Foundations and Repo Hygiene

Goal: two working build targets, testable without hardware.

Tasks:
1. Install the ARM Cortex-M0+ toolchain (VS Code Pico extension) for the
   RP2040 dev build.
2. CMake restructure:
   - `CMakePresets.json` with two presets: `dev` (PICO_BOARD=pico,
     platform rp2040, serial stdio) and `product` (custom board header for
     RP2354B, platform rp2350, RISC-V toolchain, `PICO_FLASH_SIZE_BYTES=2MB`).
   - Split the monolithic `SPIComputerOS.c` into `core0/` and `core1/`
     translation units (still single binary, both cores in one image).
3. `board_config.h`: pin maps and capability macros behind
   `#if defined(PICO_RP2350)`; dev board = RS232 + SD only.
4. Move the scratch host test into `tests/host/` (mock `ff.h`, mock SD,
   test main exercising the Lua bridge) with a CMake host target.
5. Move SD pins on the product config to SPI1 GP8–11 (HSTX conflict).

Acceptance:
- `cmake --preset dev && cmake --build` produces a UF2 that boots on the
  RP2040 module, prints over RS232, mounts the SD card, runs `os.lua`.
- Host tests run and pass on the developer machine without hardware.
- Product preset configures cleanly (build may fail until Phase 1 lands
  scanvideo — acceptable; keep it compiling by stubbing the video module).

Rationale:
- Two presets make the board split explicit; platform ifdefs are the only
  capability mechanism (no custom feature flags to drift out of sync).
- Host tests turn the bridge code from "trust me" into a fast feedback
  loop; they cost one mock header per subsystem.

---

## Phase 1 — Video Subsystem (core 0)

Goal: fixed 640x480 output, all tile modes + mode 10, serial mirror.

Tasks:
1. ~~Vendor `pico_scanvideo_dpi`~~ deferred to Phase 7 (no hardware to
   validate; the renderer and mirror are implemented and host-tested).
2. ~~Fixed timing: 640x480@60~~ deferred with task 1 (Phase 7).
3. Video state in `system_state_t`:
   ```c
   typedef struct {
       uint8_t  mode;            // 0,1,2,3,10
       uint8_t  *char_map[2];    // double-buffered, swapped at vsync
       uint8_t  *attr_map[2];    // 1 bit invert | 3 bits colour | 4 spare
       uint8_t  tiles[256][8][8];// RAM override set (see rationale)
       uint8_t  *framebuf[2];    // mode 10 only, 320x240
       uint8_t  active;          // which buffer is being written by Lua
       uint32_t palette[256];    // RGB888, mode 10 (and attr colour table)
       volatile uint8_t vsync_version; // incremented by renderer each frame
   } video_state_t;
   ```
4. Scanline renderer (RAM-resident, `__not_in_flash_func`):
   - `render_line(y, out[640*3])`: `row=y/8; sub=y%8`; per column fetch
     `tiles[char_map[row][col]][sub][0..7]`, apply attr (invert, colour),
     emit 3 bytes/pixel (RGB888). Modes 0/1/10 double each pixel and line
     (2x scale); modes 2/3 native.
   - Ping-pong scanline buffers, DMA to HSTX, using scanvideo's scanline
     callback machinery.
5. Mode switching (`ScreenMode`): allocate the needed maps from static
   pools (no malloc), free/return the previous ones, reset active buffer.
   Mode 10 allocates its double framebuffer from a dedicated pool.
6. Lua API module `screen_lua.c`:
   - `ScreenMode(mode)`
   - `ScreenOut(x, y, char)` — also an attribute setter
     (`ScreenAttr(x, y, flags)` or a combined variant)
   - `ScreenDefineTile(index, bytes)` — writes the RAM override set
   - `ScreenPalette(index, r, g, b)` / bulk `ScreenPaletteSet(t)`
   - `ScreenClear([char])`
7. Serial mirror (all boards, always compiled on dev board): core 0 emits
   the text frame protocol from the active char map over the RS232 UART —
   `resolution=`/`foreground=`/`background=` headers on connect and on
   change, one `data=` line per frame (1200 tile values), `tile=` lines
   when Lua redefines tiles. Only for modes 0/1.

Acceptance:
- Host test: renderer produces correct scanline bytes for a known char map
  (golden output), including 2x scaling, invert and colour attributes.
- On the RP2040 dev board: serial mirror shows `os.lua` output as a 40x30
  text frame updating correctly.
- On product board (Phase 7 hardware): 640x480 image on a monitor for all
  tile modes; mode 10 displays framebuffer content.

Rationale:
- Fixed timing means the monitor never resyncs; one code path for sync.
- Double-buffered maps + vsync pointer swap = Lua never blocks the
  renderer and the renderer never sees a half-updated map. The attribute
  map is swapped in the same operation (a small struct of pointers, one
  atomic exchange).
- Tile set lives in **flash** by default (the ASCII font ROM is read-only
  data); `ScreenDefineTile` writes into a RAM override set so per-program
  tile edits are cheap and restorable. Saves 16 KB RAM when unused.
- Scanline rendering keeps RAM free for Lua states; the 640-byte working
  buffers are negligible.

---

## Phase 2 — RS232 Terminal Application (macOS)

Goal: drive the dev board over the serial link from a Mac: render the
tile stream and send keyboard input.

Tasks:
1. **Native macOS app (`../terminal/macos`, sibling folder; SwiftUI):** serial access via IOKit
   (or ORSSerialPort). A virtual display renders the tile stream with the
   font8x8 ROM font (ASCII-aligned, same tiles as the firmware) and the
   fg/bg colours from the stream.
2. **Protocol (text, line-oriented, see AGENTS.md):**
   - device → app: `resolution=40x30`, `foreground=…`, `background=…`
     (on connect and on change; unchanged values are ignored),
     `tile=<index>,<8 bytes>` (custom tiles, on connect and on
     redefine), and one `data=` line per frame with the 1200 tile
     values row-major (decimal or 0x hex, cast to uchar).
   - app → device: `input=<code>` (board synthesises down+up);
     `input=<code>,1` / `input=<code>,0` for explicit press/release
     (modifiers).
3. **Rendering:** repaint on each complete `data=` line; keep the last
   frame while the stream updates; tolerate partial lines mid-frame.
4. **Serial plumbing:** port picker, 115200 8N1, connect/disconnect,
   and an optional raw byte log for debugging the stream protocol.

Acceptance:
- On the dev board: the app renders the tile stream live and typed keys
  produce correct key events visible in Lua `tick()`.
- The same app drives the product board over real RS232 (USB-serial
  adaptor).

Rationale:
- A small native app is the right size for a serial terminal: native
  serial access, low-latency key delivery, and the same font data as the
  firmware. The web IDE is a separate project, so this app stays
  deliberately single-purpose.

---

## Phase 3 — Input Subsystem (core 0)

Goal: keyboard + joysticks + RS232 in, one unified event stream.

Tasks:
1. Unified event struct:
   ```c
   typedef struct {
       uint8_t  type;      // KEY | CONTROL1 | CONTROL2
       uint8_t  key;       // ASCII key code for KEY events
       uint8_t  mods;      // SHIFT | CTRL | C= | RESTORE bits
       uint8_t  pressed;   // 1 = down, 0 = up
       uint8_t  ctrl;      // which controller
       uint8_t  dirs;      // UP|DOWN|LEFT|RIGHT|FIRE bitmask
   } input_event_t;
   ```
2. Keyboard matrix scanner on core 0: timer-driven scan (1 kHz), 16 GPIOs
   (8 rows out, 8 cols in), debounce state machine per key. Map matrix
   positions to ASCII via a lookup table (with shift table for capitals and
   symbols; C= and CTRL combinations). Accept ghosting limitations
   (shift+letter fine, 3-key chords not guaranteed).
3. Joystick reader: poll DSUB9 lines in the same 1 kHz tick, edge-detect
   direction/fire changes into events.
4. RS232 keyboard-in: UART RX on core 0 (IRQ or polled), parses
   `input=` lines → KEY events. A bare code synthesises down+up on the
   board; `,1` / `,0` suffixes give explicit press/release (modifiers).
5. SPSC event queue (ring buffer in `system_state_t`): core 0 pushes,
   core 1 drains between ticks. Drop-oldest policy on overflow.

Acceptance:
- Host test: scanner debounce logic (simulated GPIO pattern) produces the
  correct key events; queue never corrupts under producer floods.
- On dev board: typing in a terminal app produces correct ASCII events
  visible in Lua `tick()`.

Rationale:
- One event schema keeps the Lua side board-agnostic: the C64 matrix and
  the serial port feed the same queue. Debounce and decoding are core 0's
  job by design.
- 1 kHz scan is far beyond human input rates and keeps debounce trivial
  (N-samples-of-M window); timer IRQ is core 0's anyway.

---

## Phase 4 — Dual-Core Runtime and SD RPC

Goal: core 1 scheduler + strict core 0 I/O ownership.

Tasks:
1. Boot: core 0 inits clocks, stdio, SD, video, input; then
   `multicore_launch_core1()`. Core 1 creates the Lua states and enters
   the scheduler loop. IRQs (UART, SPI, timers, HSTX/DMA) stay on core 0.
2. RPC transport: two fixed queues (request, response) + a semaphore for
   core 1 to block on completion. Small fixed request/response buffers
   (paths ≤ 128 bytes, data chunks ≤ 4 KB staged through a shared buffer).
3. Move FatFs fully behind the RPC:
   - Core 0 runs the FatFs calls (`f_open`/`f_read`/...) and owns all
     `FIL` handles.
   - `fs_lua.c` (renamed from `fatfs_lua.c`) sends RPCs; Lua file objects
     hold a handle id, not a `FIL*`. Add `fs.readall(path)` and
     `fs.writeall(path, data)`.
   - Script loading (`dofile`/`loadfile`/`require`) also goes through the
     RPC (read file via core 0, compile on core 1).
4. Watchdog: core 0 feeds the hardware watchdog only if core 1's heartbeat
   counter is advancing (stuck VM = system reset).
5. Core 1 scheduler loop:
   ```
   for (;;) {
       heartbeat++;
       drain input queue -> current program's event state
       now = time_us_64();
       if (timer due) run due timer callbacks
       else tick();
   }
   ```

Acceptance:
- Host test: RPC protocol round-trip under a mock core 0; fs module tests
  pass over the RPC path instead of direct FatFs calls.
- On dev board: `os.lua` runs with SD access via RPC; pulling the SD card
  produces clean Lua errors, not crashes.

Rationale:
- Strict single-owner I/O kills every reentrancy/locking question in FatFs
  and SPI at the root. Core 1 blocking on a semaphore costs nothing for
  this workload (file ops are user-paced).
- Heartbeat-gated watchdog catches both core-0 and core-1 wedges with one
  mechanism.

---

## Phase 5 — Process Model

Goal: stack of Lua programs with setup/tick/finish, timers, save/restore.

Tasks:
1. Process struct:
   ```c
   typedef struct program_t {
       uint32_t pid;
       lua_State *L;
       int  tick_ref, setup_ref, finish_ref;  // registry refs
       video_snapshot_t *snapshot;            // saved video state
       timer_t *timers;                       // deadline list
       input_event_t *events;                 // ring buffer for tick()
       struct program_t *next;
   } program_t;
   ```
   A fixed pool of `program_t` (e.g. 4) caps stack depth and memory.
2. Memory policy: per-program Lua heap budget via a custom allocator
   (`lua_newstate` with a cap, e.g. 96 KB heap + 16 KB stack); video
   snapshots come from a fixed pool sized for the worst case (mode 2/3:
   9.6 KB maps + up to 16 KB tile overrides + palette).
3. Video snapshot/restore: on pause, copy current maps/tiles/palette into
   the snapshot and set a fresh video state for the new program; on
   restore, copy back and swap the active buffers. (Copy is ≤ 30 KB,
   one-off, and avoids the aliasing bugs of shared buffers.)
4. Program API in C: `program_launch(path)`, `program_exit()`,
   `program_pause()`. Each `tick()` runs under `lua_pcall`; errors print
   via stdio and terminate the program (shell resumes).
5. Timers: per-program deadline list (array scan is fine), 1 ms resolution,
   `TimerCreate(fn, interval_ms [, oneshot])`, `TimerStop(id)`. Deadlines
   are absolute `time_us_64`; on resume, shift all deadlines by the pause
   duration (no catch-up bursts).
6. Sys module (`sys_lua.c`): `TimeNow()` (ms), `ExitProgram()`,
   `Pid()`, maybe `Launch(path)`.
7. Shell: `os.lua` becomes the CLI — `dir`, `run <prog>`, `quit`. The
   shell is just another program (pid 0).

Acceptance:
- Host test: launch A → A launches B → B exits → A's tick resumes with its
  video state restored; timers fire correctly across pause/resume; a
  throwing tick terminates the program and resumes the parent.
- On dev board: shell runs over serial, runs a demo program, returns to
  the shell with screen restored.

Rationale:
- Copy-on-switch snapshots are simple and memory-bounded; aliasing/shared
  buffers are the classic source of subtle video bugs and aren't worth it
  for ≤ 30 KB.
- Fixed pools (programs, snapshots) mean the OS has no fragmentation
  story at all — allocation failure = "can't launch", a clean Lua error.
- pcall-per-tick isolates faults: a bad program can never take down the
  shell or the OS.

---

## Phase 6 — Editor

Goal: the first real application, exercising every subsystem.

Tasks:
1. `fs.readall` / `fs.writeall` over RPC (Phase 4 already adds them).
2. Editor program (`editor/` folder on SD): load file into RAM (gap
   buffer), render via tile modes, cursor blink via a 500 ms timer,
   keyboard events drive movement/insert/delete, Save writes back through
   RPC.
3. Key handling details: C64 cursor keys (shifted layout) mapped to
   movement; shift+letter capitals; INST/DEL insert/delete semantics.
4. Polish: status line (filename, row/col), dirty flag, save confirmation.

Acceptance:
- On dev board (serial): full edit/save/load cycle of a Lua file,
  verified by reloading it in the shell.

Rationale:
- In-RAM editing makes insert O(1) amortised (gap buffer) and avoids
  wear-leveling concerns on the card; whole-file save is one RPC.

---

## Phase 7 — Product Board Bring-Up (RP2354B)

Goal: HDMI + full input on real hardware.

Tasks:
1. Board header for RP2354B (flash size, pin map, platform rp2350).
2. Validate HSTX/scanvideo timing on a real monitor.
3. Validate SD on SPI1 GP8–11 and both SPI busses coexisting.
4. Validate matrix + joystick wiring, debounce tuning on real keys.
5. Burn-in: soak test (editor + game loop) with watchdog armed.

---

## Phase 8 — Audio Subsystem

Goal: Lua-authored tones and looping multi-channel scores over HDMI audio
(product board; the RP2040 dev board has no audio output path).

Status: the engine, Lua API, per-program state and WAV loading are
implemented and host-tested (`audio.c`, `sound_lua.c`,
`spicomputer_audio_tests`). The HDMI data-island feed lands with the
Phase 7 hardware bring-up (task 1), which is the only hardware-gated part.

Tasks:
1. Settle the single HSTX owner (Phase 7 decision): `pico_hdmi` (HSTX HDMI
   including audio data islands, Unlicense) vs `pico_scanvideo_dpi` (video
   only) plus ported data-island code. Vendor the winner and keep it behind
   `video_hw.c` / `audio_hw.c` so only one file each touches it. *(pending:
   hardware)*
2. Core 0 realtime engine: 8-channel integer mixer at 44.1 kHz (waveform +
   envelope voices), score cursor with looping, one-shot effect voices,
   master volume; feeds the HDMI audio packet queue (silence + starvation
   counter on underrun). **Implemented** as the platform-neutral producer
   (`audio_mix`); the HDMI packet feed lands with task 1.
3. Lua API (`sound_lua.c`): **implemented** (see `lua.md`). `*Define`
   compiles Lua tables into flat per-program buffers (outside the 64 KB Lua
   heap), published to core 0 by pointer swap.
4. Per-program audio state (like video): **implemented** - publish on
   program switch, silence paused programs, resume score position, free on
   exit.
5. WAV sample loading: **implemented** - `SoundLoad(path)` streams PCM
   (8/16-bit, mono/stereo) via the fs RPC into a 64 KB per-program pool;
   IMA ADPCM remains a possible later size optimisation.
6. Host tests: **implemented** - oscillator/envelope/mixer behaviour, score
   scheduling and loop wrap, pause/resume across a program launch, WAV
   parsing over the mock SD, and the full Lua API.

Acceptance:
- ~~Host tests pass.~~ (done)
- On the product board: a defined score plays and loops over HDMI, one-shot
  effects mix over it, and editor/game use causes no dropouts.

Rationale:
- Core 0 owns every sample: timing is sample-accurate and cannot be
  perturbed by Lua ticks, GC or SD RPC stalls on core 1.
- Scores are authored as Lua tables (readable, diffable) but compiled to
  flat buffers, so the realtime path never enters Lua.
- HDMI data-island audio keeps the pin budget intact; I2S/PWM audio stays a
  fallback only if HSTX data islands prove unreliable on real sinks.

---

## Memory Budget (RP2354B, 520 KB)

| Consumer | Size |
|---|---|
| Core 0: scanline buffers, DMA, event/RPC queues | ~12 KB |
| Mode 10 double framebuffers (active only in mode 10) | 153.6 KB |
| Tile override RAM set | 16 KB |
| Audio per program: instruments + scores + 1024-event pool, heap-allocated like video | ~15 KB |
| WAV sample pool per program (malloc'd on first SoundLoad) | up to 64 KB |
| Char+attr maps (mode 2/3 worst case, double-buffered) | 19.2 KB |
| Video state per program (heap-allocated on launch, 4 × ~28 KB on demand) | ~112 KB worst case |
| Lua states: 4 programs × (64 KB heap cap + VM) | ~280 KB worst case |
| Font ROM (flash) | 16 KB (flash) |

Mode 10 and deep program stacks are mutually constrained in practice:
a pixel-mode game holds no paused programs, and the shell/editor stack
lives comfortably in tile modes. Documented as a policy: **mode 10 is
single-program; launching from a mode 10 program is rejected with a Lua
error.** (Revisit if a real need appears.)

## Flash Budget (2 MB)

Firmware + Lua + FatFs + scanvideo ≈ 600 KB, leaving > 1 MB for the font
ROM, future embedded assets, and (optionally) littlefs config storage.

## Risk Register

| Risk | Mitigation |
|---|---|
| scanvideo on RP2350 is immature/changes API | Vendor a pinned revision; wrap behind `video.c` so only one file touches it |
| C64 matrix ghosting surprises users | Document; offer serial keyboard as the reliable path |
| ~3 fps full-frame serial mirror too slow for editor UX | Editor redraws dirty cells only; or add a changed-cells delta record to the text protocol later (backwards-compatible new record type) |
| Lua heap fragmentation across long sessions | Bounded per-program allocators + program exit frees everything |
| HSTX + SPI1 coexistence bugs on real silicon | Phase 7 burn-in; SD access and video run simultaneously from day one in tests |
| Terminal app drifts from firmware behaviour | Parser + font are canonical in the OS repo and vendored into the app; `sync-protocol.sh` refreshes the copies, golden stream vectors live in `tests/host` |
| macOS serial access friction (driver quirks, sandbox) | ORSSerialPort/IOKit is the well-trodden path; USB-serial adaptors work without custom drivers |

# SPIConputer (working title)

> A retro computer whose system bus is SPI. Every peripheral is an
> autonomous node with its own microcontroller; the main CPU sends it a
> command and the node carries it out without further CPU involvement.

## Status

Design phase. Nothing is built yet. This README is the living design
document: decisions go in the [Decision log](#decision-log), unresolved
items live in [Open questions](#open-questions).

## Concept

- **Bus = SPI.** SCK, MOSI and MISO are shared across all nodes; each
  node gets its own chip-select (SS) line from the main CPU.
- **Autonomous peripherals.** Each node (video, storage, IO, sound,
  console) has its own MCU (ATmega328 or similar). The main CPU issues
  high-level commands ("draw this sprite", "read sector N", "play this
  note") and moves on. Nodes report completion asynchronously.
- **C64 form factor.** The board fits inside a Commodore 64 shell,
  reusing its keyboard and (ideally) its port cutouts.

## Architecture overview

```
                                  +----------------------+
                                  |  MAIN CPU (master)   |
                                  |  MCU: TBD            |
                                  +--+--+--+--+--+--+----+
                                     |  |  |  |  |  |
        Bus: SCK + MOSI + MISO shared; one SS per node; IRQ per node (TBD)
                                     |  |  |  |  |  |
        +----------------------------+  |  |  |  |  +--------------------------+
        |                               |  |  |  |                             |
  +-----+--------+              +-------+  |  |  +-------+             +-------+--------+
  | VIDEO NODE   |              | IO NODE |  |  | STORAGE |             | SOUND NODE     |
  | ATmega2560   |              | ATmega  |  |  | NODE    |             | ATmega328 (TBD)|
  +-----+--------+              +---+-----+  |  +----+----+             +-------+--------+
        |                           |        |       |                         |
  2nd SPI -> Flash           C64 keyboard    |   SD card (SPI)           Audio out (TBD)
  (tile/glyph data)          Joystick x2     |
        |                           |        |
       VGA (DB15)                   |        |
                                    |   +----+--------+
                                    |   | CONSOLE NODE |
                                    |   | RS232 / TBD  |
                                    |   +-------------+
                                    |
                          +---------+---------+
                          | EXPANSION (SDI)   |
                          | bus passthrough   |
                          +-------------------+
```

## System bus

### Signals

| Signal | Direction | Notes |
| ------ | --------- | ----- |
| SCK    | master -> all | Bus clock |
| MOSI   | master -> all | Master out, slave in |
| MISO   | all -> master | Shared; slaves tri-state when not selected |
| SS_n   | master -> one node each | Dedicated chip-select per node |
| IRQ_n  | node -> master | Async completion signal (scheme TBD) |

### Protocol (proposal, unverified)

- Every transaction: master asserts the node's SS, sends a 1-byte
  command, an optional length byte, then payload. The slave responds
  with status byte(s) in the same transfer.
- Each node exposes a small **register map**: writing a command
  register starts an autonomous operation; a status register reports
  idle/busy/done/error; data registers move payloads.
- Completion: node asserts IRQ_n when done (or master polls status).
  See [Open questions](#open-questions).

### Timing and electrical gotchas

- **Slave clock limit:** on classic AVRs, SPI *slave* mode tops out at
  Fosc/4 (4 MHz at 16 MHz Fosc) even though *master* mode can run at
  Fosc/2 (8 MHz). The bus clock is therefore realistically **4 MHz**
  unless a node runs a faster crystal or a different SPI peripheral.
- **Shared MISO:** AVR SPI hardware tri-states MISO while SS is
  deasserted, which is what makes the shared bus work. Firmware must
  keep the SPI peripheral in slave mode; don't reuse MISO as a GPIO
  output.
- **Master SS pin:** in AVR SPI master mode the SS pin must be an
  output (or held high) or the master's own SPI can drop into slave
  mode on a false edge.
- Keep traces short; bus speed is modest but the bus crosses multiple
  boards/connectors.

## Nodes

### Main CPU (master)

- MCU: **TBD**. Candidates: ATmega2560 (fits the all-AVR theme and has
  enough pins for per-node SS + IRQ), or something bigger.
- Owns the bus, dispatches commands, runs the OS/shell/BASIC.
- Needs enough GPIO for: one SS per node, one IRQ per node, any
  console-side RS232 if not handled by a node.

### Video node

- MCU: **ATmega2560-16AU** (specified).
- Output: **VGA via DB15** (rear panel).
- Rendering: **text/sprite system**. Tile map and sprite list live in
  SRAM; tile/glyph bitmaps live in an external **SPI flash** on a
  *second* SPI bus (separate from the system bus).
- Firmware renders scanlines, not a CPU-driven framebuffer.

**Timing budget** (this constrains every video decision):

- Pixel clock comes from an AVR shift register (SPI/USART in master
  mode) = Fosc/2 = **8 MHz** at 16 MHz Fosc.
- One VGA line (31.78 µs) = ~254 pixel clocks at 8 MHz, so visible
  width is roughly **160-250 px** at 1 bit per pixel, packed 8 px per
  byte (~25-30 bytes per line, ~1 µs per byte shifted out).
- Flash reads also max out at ~1 byte/µs (8 MHz SPI). Fetching tile
  data on the fly leaves zero slack, so glyphs/tile rows must be
  **cached into SRAM during blanking** (8 KB SRAM on the 2560).
- Resolutions, colour depth (e.g. 3-bit RGB via 3 pins vs. resistor
  DAC) and sprite count are all [open questions](#open-questions).

### IO node

- MCU: ATmega328 or similar.
- Drives the **C64 keyboard** (8x8 matrix, needs 16 GPIOs + pull-ups)
  via the internal connector, and two **DB9 joystick ports**
  (Atari/C64 standard: 4 directions + fire, active-low, pulled high).
- Scans/debounces locally; the main CPU reads key/joystick *events*,
  not raw matrix state.

### Storage node

- MCU: ATmega328 or similar.
- **SD card in SPI mode** (rear panel slot).
- Exposes block read/write; FAT layer either here or on the main CPU.
- Gotcha: SD cards are **3.3 V logic** — the node needs a 3.3 V rail
  (LDO) and level shifting on MOSI/SCK/SS (MISO can usually feed 5 V
  inputs directly through a divider). Classic AVRs are 5 V parts.

### Console node

- **RS232** serial console (rear panel, DB9).
- Open: true RS232 levels (needs a MAX232-style transceiver) vs. TTL
  serial vs. USB (FTDI/CH340). This node may end up merged into the IO
  node.

### Sound node

- MCU: ATmega328 or similar (TBD).
- Receives note/effect commands and generates audio autonomously;
  output via PWM + RC filter, then amp/jack (TBD).
- Open: chiptune-style synth, sample playback, or a SID-inspired voice
  set.

### Expansion (SDI)

- Rear-panel expansion connector. Intended as a passthrough of the
  system bus (SCK/MOSI/MISO + an SS/IRQ pair + power) so new nodes can
  hang off the bus.
- **"SDI" needs defining** — see [Open questions](#open-questions).

## Connectors

| Connector | Location | Standard / notes |
| --------- | -------- | ---------------- |
| VGA       | rear     | DB15, driven by video node |
| Console   | rear     | DB9 RS232 (or USB) — TBD |
| Expansion | rear     | SDI — bus passthrough, pinout TBD |
| SD card   | rear/side | microSD slot, SPI mode |
| Joystick x2 | rear/side | DB9, Atari/C64 standard |
| Keyboard  | internal | C64 keyboard connector, matrix scan by IO node |
| Power     | rear     | 5 V barrel jack — TBD |

## Form factor

- Board must fit a **C64 shell**; port cutouts should line up with the
  case openings. Open: which C64 revision (breadbin vs. 64C) — they
  have different keyboard connectors and cutouts.

## Power

- Single **5 V** rail for all nodes; local 3.3 V LDO on the storage
  node (and anywhere else 3.3 V logic appears). Total draw is small
  (a handful of AVRs + flash + SD), but budget should be verified once
  nodes are prototyped.

## Firmware conventions (proposed)

- Every node: **command loop** — SPI slave ISR receives command frames,
  main loop executes the current task, sets status, raises IRQ.
- Shared node firmware skeleton/library would avoid re-implementing the
  bus layer per node (TBD once toolchain is chosen).
- Each board exposes an **ICSP/ISP header** for initial programming
  and bootloader flashing.

## Development tooling

TBD. Candidates: avr-gcc + avrdude directly, PlatformIO, or Arduino
CLI. Decide in the [Decision log](#decision-log) and document the
canonical build/flash commands here once real code exists.

## Decision log

| # | Decision | Rationale | Date |
| - | -------- | --------- | ---- |
|   | *(none yet)* |  |  |

## Open questions

1. **Main CPU** — which MCU drives the bus and runs the OS?
2. **Bus protocol** — exact command framing; register-map layout per
   node; IRQ scheme (one line per node vs. shared open-collector).
3. **Video modes** — target resolution(s), colour depth, sprite count,
   tile cache strategy within the 8 MHz pixel-clock budget.
4. **SDI definition** — what exactly does the expansion connector
   carry, and what can plug into it?
5. **Sound architecture** — synth vs. samples, voice count, output
   stage (amp, jack on case?).
6. **Console** — true RS232 levels, TTL, or USB?
7. **C64 case revision** — breadbin or 64C (keyboard connector,
   cutouts, mounting holes differ).
8. **Toolchain** — avr-gcc / PlatformIO / Arduino, and the flash
   workflow for each node.
9. **Firmware framework** — shared bus-layer library across nodes, or
   per-node implementations?

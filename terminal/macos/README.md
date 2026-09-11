# SPI Terminal — macOS RS232 Terminal

Native macOS app that drives the SPIComputer OS over its serial
development link: renders the tile-stream video output and sends
keypresses back as emulated keyboard input.

## Build & run

The app is a sibling folder of the OS repo (`SPIConputer/terminal/macos`):

```bash
cd terminal/macos
swift run SPITerminal
```

Requires Xcode (macOS 13+). No external dependencies.

## Usage

1. Plug in the board's USB-serial adaptor (or RS232 cable + adaptor).
2. Pick the port (e.g. `cu.usbserial-0001`) and press **Connect**.
   The link is fixed at 115200 8N1.
3. Click the display once to give it keyboard focus, then type.
4. **Log bytes** writes both directions of the raw stream to a file for
   protocol debugging.

## Wire protocol (device -> app)

Line-oriented text records, one per line, LF-terminated:

```
resolution=40x30      sent at connect and on change
foreground=yellow     sent at connect and on change
background=darkblue   sent at connect and on change
tile=65,0,1,2,3,4,5,6,7   custom glyph (8 bytes); ROM font otherwise
data=0,0,0,36,38,41,87,0,0,76,98,...   one frame, row-major, WxH values
```

- Values are decimal or `0x` hex, cast to `uchar` (tile index).
- The app repaints on each complete `data=` line and holds the last
  frame in between; partial lines mid-frame are ignored.
- Colour names follow the C64 16-colour palette (black, white, red,
  cyan, purple/violet/magenta, green, blue/darkblue, yellow, orange,
  brown, lightred, darkgrey/grey, lightgreen, lightblue, lightgrey,
  plus grey/gray aliases). Unknown names and malformed lines are
  ignored.
- Unrecognised glyph indexes render with the bundled font8x8 ROM font
  (ASCII-aligned, same font the OS will use), so plain ASCII frames
  are meaningful without any `tile=` records.

## Key mapping (app -> device)

| Key | Lines sent |
|---|---|
| Printable ASCII (shifted where held) | `input=<code>` |
| Return / Enter | `input=13` |
| Tab | `input=9` |
| Backspace / Forward delete | `input=8` / `input=127` |
| Escape | `input=27` |
| Ctrl+letter | `input=<1-26>` |
| Arrow keys | `input=27`, `input=91`, `input=65/66/68/67` (ESC [ A/B/D/C) |

A bare `input=<code>` is synthesised into down+up on the board;
`input=<code>,1` / `input=<code>,0` are the explicit press/release forms
(reserved for modifiers, not yet emitted by the app). Cmd shortcuts stay
with the system.

## Layout

- `Sources/TerminalC/` — vendored copies of the OS serial-mirror parser
  (`protocol.c`, canonical in `../../os/`) + public-domain font8x8. Run
  `./sync-protocol.sh` after changing the protocol or font on the OS
  side. The parser is exercised by the golden-vector host tests in the
  OS repo (`tests/host`, target `spicomputer_protocol_tests`).
- `Sources/SPITerminal/` — SwiftUI app: serial port (termios),
  frame rendering, key capture.

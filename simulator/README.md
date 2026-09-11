# SPIComputer OS — Desktop Simulator

Lives next to `os/` and `terminal/` (it is not part of the OS itself).
Runs the real OS C code on macOS with the hardware layers replaced by
desktop implementations, so programs can be developed and tested without
the board:

| OS component | Simulator replacement |
|---|---|
| Core 0 FatFs / SD card (`fs_core0.c`) | `sim_fs.c`: a real host folder (default `./sdcard`) via stdio/POSIX |
| Core 0 HSTX/HDMI rendering | `render_line()` → RGB24 texture in an SDL window |
| Core 0 HDMI audio islands | `audio_mix()` → `SDL_QueueAudio` at 44.1 kHz stereo |
| Keyboard matrix / joysticks / RS232 | SDL keyboard + game controllers → `input_event_t` queue |
| Two cores | one thread: scheduler steps with the RPC serviced inline |

The Lua core, process model, timers, input deposit, screen/sound/sys
modules, RPC transport and filesystem bridge are the actual OS sources
(the same ones the host tests exercise).

## Build & run

Requires SDL2. Run these commands from the folder that contains `os/`
and `simulator/`:

```bash
brew install sdl2
cmake -S simulator -B simulator/build
cmake --build simulator/build
./simulator/build/spicomputer_sim
```

On first run the simulator creates `./sdcard` if missing and seeds it
with the OS's `os.lua` and `editor.lua` (never overwriting existing
files). Point it elsewhere with `--sdcard DIR`; `--seed-dir DIR` chooses
the folder the seeds are copied from (defaults to the sibling `os/`
tree).

## Controls

| Input | Sends |
|---|---|
| Letters, digits, punctuation, Return, Tab, Backspace, Escape, Delete | ASCII key events (shift-aware) |
| Arrow keys, Home, F1–F7 | extended C64 key codes (`INPUT_KEY_*`) |
| F8 | RESTORE modifier |
| F9 | RUN/STOP |
| Left Alt | Commodore (C=) modifier |
| Numpad 8/2/4/6, 0 | joystick 1 (up/left/right/down, fire) |
| Game controllers | joystick 1 and 2 (d-pad + A/B fire) |

## Options

```
--sdcard DIR        virtual SD card folder (default: ./sdcard)
--seed-dir DIR      copy os.lua/editor.lua from DIR when missing
--ticks N           scheduler ticks per frame (default: 64)
--dump-frame FILE   write the final 640x480 frame as a PPM
--headless          no window/audio (smoke tests, CI)
--exit-after-ms N   quit automatically after N ms
```

`--headless --exit-after-ms 1500` boots the OS, runs the scheduler and
the RPC, prints a summary and exits 0 — useful as a smoke test.
`--dump-frame shot.ppm` captures the last rendered frame for inspection
(convert with `sips -s format png shot.ppm --out shot.png`).

## Working on programs

Programs are ordinary files in `sdcard/`; edit them with any tool. A
program is loaded when launched (shell `run foo.lua`), so changes are
picked up on the next launch; `os.lua` is read at startup. The editor's
`Ctrl+S` writes straight back into the folder.

## Behaviour notes

- Ticks run `--ticks` times per rendered frame (default 64 at ~60 fps);
  programs should compute elapsed time from `TimeNow()` as on hardware.
- Audio is mixed on the main thread and queued (~20 ms target depth), the
  same single-producer shape as core 0.
- The serial mirror (`core0/serial_mirror.c`) and hardware files are not
  compiled in; the SDL window replaces the mirror as the display.
- macOS key repeat is passed through as repeated key-down events.

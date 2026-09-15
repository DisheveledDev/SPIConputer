# SPIComputer OS — Desktop Simulator

Lives next to `system/` (it is not part of the system itself).
Runs the real OS C code on macOS with the hardware layers replaced by
desktop implementations, so programs can be developed and tested without
the board:

| OS component | Simulator replacement |
|---|---|
| Core 0 FatFs / SD card (`fs_core0.c`) | `sim_fs.c`: a real host folder (default `./sdcard`) via stdio/POSIX |
| Core 0 HSTX/HDMI rendering | `render_line()` → RGB24 texture in an SDL window |
| Core 0 HDMI audio islands | `audio_mix()` → `SDL_QueueAudio` at 44.1 kHz stereo |
| Keyboard matrix / joysticks | SDL keyboard + game controllers → `input_event_t` queue |
| Two cores | one thread: scheduler steps with the RPC serviced inline |

The Lua core, process model, timers, input deposit, screen/sound/sys
modules, RPC transport and filesystem bridge are the actual OS sources
(the same ones the host tests exercise).

## Build & run

Requires SDL2. Run these commands from the folder that contains `system/`
and `simulator/`:

```bash
brew install sdl2
cmake -S simulator -B simulator/build
cmake --build simulator/build
./simulator/build/spicomputer_sim
```

On first run the simulator creates `sdcard/` **next to the simulator
binary** (e.g. `simulator/build/sdcard`) if it is missing, including the
protected `core/`, installed-app `apps/`, and writable `data/` directories.
Copy system programs into `core/`, applications into `apps/`, and user files
into `data/`. Basing it on the executable keeps it the same folder however
you launch the simulator; `--sdcard DIR` points it elsewhere.

With a `boot.lua` or `boot.prg` that starts the shell you land at the SPIOS
`READY.` prompt; `--boot <program>` boots a source or compiled program directly instead.

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

The simulator window is resizable. Its 640x480 output is always presented at
4:3, with letterboxing when the window shape is wider or taller. This makes
80-column text readable at larger window sizes without changing the logical
screen geometry.

## Options

```
--sdcard DIR        virtual SD card folder (default: <simulator dir>/sdcard)
--boot FILE         program to boot (default: core/boot.lua)
--ticks N           at most N scheduler ticks per frame (default: 64)
--dump-frame FILE   write the final 640x480 frame as a PPM
--dump-text FILE    write the final text screen: 30 lines of 40 chars, overlay
                    on top, '#' for non-ASCII codes, '|' then '^' under inverted cells
--check FILE        compile FILE with the OS Lua and exit
--compile IN OUT    compile Lua source IN to a .prg binary chunk and exit
--strip             with --compile: leave out debug info (line numbers, local
                    names); the loaded program needs about a fifth less heap,
                    but its runtime errors carry no line numbers
--headless          no window/audio (smoke tests, CI)
--exit-after-ms N   quit automatically after N ms
--type TEXT         scripted keystrokes, one per frame (\n Return, \e Esc,
                    \u \d \l \r cursor keys, \1..\7 F1..F7, \\ backslash)
--type-delay-ms N   wait N ms after start before typing (default 500;
                    use ~3500 to type into the shell after the boot splash)
```

`--headless --exit-after-ms 1500` boots the OS, runs the scheduler and
the RPC, prints a summary and exits 0 — useful as a smoke test.
`--boot apps/foo.prg` starts a program directly instead of
`core/boot.lua` (the IDE uses this to run a project straight from its
folder, and `--boot core/os.prg` skips straight to the shell).
`--dump-frame shot.ppm` captures the last rendered frame for inspection
(convert with `sips -s format png shot.ppm --out shot.png`).

## Working on programs

Programs are ordinary files in `sdcard/`; source programs use `.lua` and
compiled Lua chunks use `.prg`. A program is loaded when launched (`run
foo` from a shell, or just `foo`), with the compiled `.prg` preferred when
both forms exist. Use `--compile source.lua output.prg` to create a chunk;
changes are picked up on the next launch;
`boot.lua` is read at startup. A program that saves files writes
straight back into the folder. In this workspace the card programs are
SPIEdit projects under `software/`; their builds (`software/*.lua`) are
what you copy into `sdcard/`.

## Behaviour notes

- Each rendered frame runs up to `--ticks` scheduler steps (default 64)
  and ends early when the 60 Hz frame clock moves on, so a program that
  paces its `tick()` with `WaitVSync()` gets one step per frame and every
  frame is drawn and has its input read. The exit summary reports both
  `ticks` and `frames`. Programs should compute elapsed time from
  `TimeNow()` as on hardware.
- Audio is mixed on the main thread and queued (~20 ms target depth), the
  same single-producer shape as core 0.
- Hardware output files are not compiled in; the SDL window replaces the
  hardware display.
- macOS key repeat is passed through as repeated key-down events.

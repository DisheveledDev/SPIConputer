# Host Tests

Run the OS's Lua↔SD bridge on the developer machine using a mock FatFs/SD
layer. No hardware needed.

```bash
cmake -S tests/host -B build-host
cmake --build build-host
./build-host/spicomputer_host_tests
./build-host/spicomputer_input_tests
./build-host/spicomputer_program_tests
./build-host/spicomputer_render_tests
./build-host/spicomputer_editor_tests
```

The main test binary compiles the real Lua 5.5 core (from `../../lua`), the
real RPC transport (`rpc.c`) and the real `fs_lua.c`/`fs_core0.c` stack,
substituting mock `ff.h` / `f_util.h` / `hw_config.h` headers for the
FatFs/SD hardware layer. The RPC wait hook runs the core 0 service inline,
exactly as the firmware main loop would.

Covered:
1. Booting a card program (fixture) through the process model
2. SD-backed `dofile` from within Lua
3. SD-backed `loadfile` semantics (function / nil+err)
4. `require()` via the SD searcher
5. The full `fs` module API (open/read/write/seek/tell/size/flush/close,
   ls/stat/exists/free/ready)
6. Error handling for missing files
7. `fs.readall` / `fs.writeall` over the RPC path
8. Ejected SD card: clean Lua errors, no crashes
9. Explicit RPC round-trip against the mock core 0 service
10. A typed command line (fixture): input events, case-insensitive
    program lookup with `fs.find`, `Execute` with arguments, QUIT

`spicomputer_input_tests` exercises the input engine (`input.c`) with
simulated GPIO patterns: SPSC queue round-trip and producer-flood drop-oldest
behaviour, matrix debounce, shift handling, joystick edge detection, and
RESTORE handling.

`spicomputer_program_tests` exercises the process model: launch/resume
with video state restore, tick-crash termination, timers (incl. pause
deadline shifting), input deposit, failed launches.

`spicomputer_render_tests` exercises the video pieces: the scanline
renderer (golden output, 2x scaling, attribute colours, three text-layer
composition, custom tiles, mode 10), and the screen Lua module.

`spicomputer_audio_tests` exercises the Phase 8 audio pieces: note
parsing, oscillators, envelopes, pan/master volume, score scheduling
and loop wrap, one-shot behaviour, PCM sample playback, WAV loading
over the mock SD, the full sound/music Lua API, and audio state
across a program launch/resume.

`spicomputer_editor_tests` boots an `editor.lua` supplied on the
command line against the mock SD card and simulates typing through the
normal event path: initial render + status line, insert, ANSI cursor
sequence, backspace, dirty save confirmation, and the file written
back to the mock SD. The editor lives in its own project outside this
repo, so the harness skips when no path is given:

```
./build-host/spicomputer_editor_tests /path/to/editor.lua
```

As more subsystems gain mock headers, add their host tests here.

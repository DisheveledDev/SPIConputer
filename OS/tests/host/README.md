# Host Tests

Run the OS's Lua↔SD bridge on the developer machine using a mock FatFs/SD
layer. No hardware needed.

```bash
cmake -S tests/host -B build-host
cmake --build build-host
./build-host/spicomputer_host_tests ../os.lua
```

The test binary compiles the real Lua 5.5 core (from `../../lua`) and the
real `fatfs_lua.c` bridge, substituting mock `ff.h` / `f_util.h` /
`hw_config.h` headers for the FatFs/SD hardware layer.

Covered:
1. `fatfs_lua_run_file` booting `os.lua`
2. SD-backed `dofile` from within Lua
3. SD-backed `loadfile` semantics (function / nil+err)
4. `require()` via the SD searcher
5. The full `fs` module API (open/read/write/seek/tell/size/flush/close,
   ls/stat/exists/free/ready)
6. Error handling for missing files

As more subsystems gain mock headers (video state, renderer, RPC
protocol), add their host tests here.

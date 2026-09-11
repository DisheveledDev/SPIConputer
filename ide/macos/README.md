# SPIComputer IDE (macOS)

A SwiftUI development environment for SPIComputer OS programs. Projects
are folders of components that build into a single `.lua` file the OS can
run; **Run** writes that file into a simulator SD card and launches the
desktop simulator.

Lives next to `os/` and `simulator/` (it is not part of the OS itself).

## Build & run

```bash
cd ide/macos
swift run SPIIDE        # or open the folder in Xcode
swift test              # core build/emission logic tests
```

The IDE finds the simulator automatically by walking up from its own
binary to the workspace folder (`simulator/build/spicomputer_sim`).
Override the path in **Settings** if needed.

## Projects

A project is a folder with a manifest and component files:

```
My Program/
  project.spiproj          manifest: project name + ordered component list
  components/
    00-header.lua          snippet component (build-only comments/constants)
    main.lua               Lua component (setup/tick/finish)
    sprites.json           tiles & palette asset
    music.json             sounds & score asset
  build/
    My-Program.lua         generated program (do not edit)
    run/sdcard/            generated simulator card for Run
```

All components are concatenated **in sidebar order** (drag to reorder)
into one Lua program with a generated header. Components are plain files
on disk — edit them with any tool.

### Component kinds

| Kind | File | Emitted as |
|---|---|---|
| Lua Code | `.lua` | verbatim |
| Snippet | `.lua` | verbatim (for build-only material) |
| Tiles & Palette | `.json` | `ScreenPaletteSet` / `ScreenDefineTile` calls inside an asset function |
| Sounds & Music | `.json` | `SoundDefine` / `MusicDefine` calls inside an asset function |

Asset components register themselves in `__spi_assets`; the generated
tail defines `ApplyAssets()`, which the starter `setup()` calls so the
assets are applied while the program's video/audio state is current.

### New project template

`New Project…` creates the commented "full" skeleton: a header snippet
and a `main.lua` with `setup()` (screen setup, asset application, a
blink timer), `tick()` (input drain with keyboard echo, joystick state,
allocation-light status line) and `finish()`.

## Run in the simulator

**Run** (⌘R) builds, then writes `build/run/sdcard/` containing the
generated program and a generated `os.lua` launcher that `Launch()`es it,
and starts `simulator/build/spicomputer_sim --sdcard …`. **Stop**
terminates it. The Build & Run console shows the build result and the
simulator's output.

## Not yet

- `.luac` output (the firmware loads text chunks only; binary chunks
  would need `luaL_loadbufferx` mode `"b"`).
- Syntax highlighting in the Lua editor (plain monospaced TextEditor).
- Undo grouping/per-component history beyond TextEditor's built-in undo.

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

The Dock icon is set at launch from `Sources/SPIIDE/Resources/AppIcon.png`
(`swift run` produces a bare executable, so there is no app bundle to
carry an icon).

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

`New Project…` always creates four components:

- **header** (snippet) — build-only documentation/constants.
- **main** (Lua) — empty `setup()`/`finish()` with comments above them
  listing the screen modes and text output (`ScreenOut`/`ScreenClear`).
- **input** (Lua) — empty `on_keypress(key, shift, ctrl, cbm, restore)`
  and `on_control(index, up, down, left, right, fire)` with comments
  describing what each parameter receives (index 0/1 for joystick 1/2).
  Both are optional; delete the one you don't need, or poll `InputPoll()`.
- **tick** (Lua) — the empty `tick()` loop with comments on where it sits
  in the lifecycle.

Components build top-to-bottom into one Lua chunk, so the locals declared
in `main` are visible to `input` and `tick`.

## Compile checking and editing

- The editor shows **line numbers** in a gutter per component file.
- After a short debounce, the IDE builds the project in memory and
  compiles it with the OS's own Lua build (`spicomputer_sim --check`), so
  what you see is exactly what the OS will load.
- Compile errors are mapped back through the build's line map: Lua's
  "line 41" in the generated file is shown as e.g. `main:13`, the error
  line is highlighted in the editor and its number turns red in the
  gutter, and the message appears in a banner and in the Build & Run log.
  When Lua only reports `<eof>` (a missing `end`), the IDE uses Lua's
  "at line N" context or its own block-structure check to point at the
  unclosed function/if/for/while/repeat instead.
- If the simulator binary cannot be found (checks run through its `--check`
  mode), an orange banner says so; the IDE looks next to itself, in the
  current directory, and along the compiled-in source path (so Xcode
  builds work too), and Settings can override it.
- **Run** refuses to start while a compile error is outstanding.
- **Autocompletion** covers Lua keywords and standard library plus the
  SPIComputer globals (`Screen*`, `Sound*`, `Music*`, `Timer*`, `Input*`,
  `fs.*`, `TimeNow`, `Launch`, `ApplyAssets`, ...). The popup appears
  shortly after you type (two or more characters), or on demand with
  Ctrl-Esc / **Edit ▸ Complete** (⌃Space); the inline macOS "automatic
  text completion" is disabled so the list is what appears.
- **Auto-indent**: Return keeps the current indentation and adds a level
  after block openers (`then`, `do`, `function`, `else`, `repeat`, `{`,
  `(`, function headers); typing `end`, `until`, `else`, `elseif`, `}` or
  `)` at the start of a line removes one level.
- **Syntax highlighting** colours comments, strings, numbers, keywords
  and known functions (temporary attributes, so undo and the text buffer
  are untouched).

## Run in the simulator

**Run** (⌘R) builds, then writes `build/run/sdcard/` containing the
generated program and a generated `os.lua` launcher that `Launch()`es it,
and starts `simulator/build/spicomputer_sim --sdcard …`. **Stop**
terminates it. The Build & Run console shows the build result and the
simulator's output.

## Not yet

- `.luac` output (the firmware loads text chunks only; binary chunks
  would need `luaL_loadbufferx` mode `"b"`).
- Signature help / hover documentation for the SPIComputer API.

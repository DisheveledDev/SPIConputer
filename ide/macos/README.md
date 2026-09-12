# SPIComputer IDE (macOS)

A SwiftUI development environment for SPIComputer OS programs. Projects
are folders of components that build into matching `.lua` source and `.prg`
compiled outputs; **Run** writes both files into a simulator SD card and
launches the compiled program in the desktop simulator.

Lives next to `system/` and `simulator/` (it is not part of the system
itself).

## Build & run

```bash
cd ide/macos
swift run SPIIDE        # development launch
swift test              # core build/emission logic tests
swift run spibuild <project-folder>   # headless Build (see below)
./scripts/build-app.sh  # release .app with bundled simulator
```

The IDE finds the simulator automatically from the bundled app resource
(`Contents/Resources/simulator/spicomputer_sim`). Development launches also
search the workspace at `simulator/build/spicomputer_sim`. Override the path
in **Settings** if needed.

`./scripts/build-app.sh` builds a release `SPIComputer IDE.app` in `dist/`.
It builds the simulator with CMake, embeds the simulator and SDL2 runtime,
and includes the SwiftPM resource bundle and app metadata.

`spibuild` is the headless counterpart of the IDE's Build button: it
loads `project.spiproj`, compiles the components and writes the program
to the project's output directory, so projects can be built from a
terminal or CI as well as from the IDE:

```bash
swift run spibuild /path/to/My-Project
```

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
    My-Program.lua         generated source program (do not edit)
    My-Program.prg         generated Lua bytecode (do not edit)
    run/sdcard/core/       protected system area
    run/sdcard/apps/       compiled project program
    run/sdcard/data/       writable user area
```

All components are concatenated **in sidebar order** (drag to reorder)
into one Lua program with a generated header. New projects can be
interactive applications with their own video/audio state or noninteractive
utilities. Utilities run in an isolated Lua state without allocating a video
stack and return text through `UtilityResult(ok, message)`.
Components are plain files on disk — edit them with any tool.

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
- Open Lua and snippet components are polled for external changes. Agent or
  editor updates are reloaded automatically when the IDE has no unsaved edits;
  conflicting changes are kept in the editor and reported in the console.
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
  `fs.*`, `TimeNow`, `Launch`, `ApplyAssets`, ...). The list appears
  shortly after you type (two or more characters), or on demand with
  Ctrl-Esc / **Edit ▸ Complete** (⌃Space), which also steps through the
  list once open. Up/Down choose, Tab or Return insert, Esc dismisses.
  Backspace only dismisses and always deletes, and deleting never
  reopens the list; the inline macOS "automatic text completion" is
  disabled so this list is what appears.
- **Parameter help**: while the caret is inside a call's argument list
  (`ScreenOut(1, `), a strip under the caret shows the signature with
  the current parameter emphasised, following the caret until the call
  is closed. It covers the SPIComputer API, the Lua standard library and
  the `fs` file methods.
- **Auto-indent**: Return keeps the current indentation and adds a level
  after block openers (`then`, `do`, `function`, `else`, `repeat`, `{`,
  `(`, function headers); typing `end`, `until`, `else`, `elseif`, `}` or
  `)` at the start of a line removes one level.
- **Syntax highlighting** colours comments, strings, numbers, keywords
  and known functions (temporary attributes, so undo and the text buffer
  are untouched).

## Run in the simulator

**Run** (⌘R) builds both outputs, writes them into `build/run/sdcard/` and
starts `simulator/build/spicomputer_sim --sdcard … --boot <program>`, so the
simulator boots the compiled `.prg` directly. **Stop** terminates it. The
Build & Run console shows the build result and the simulator's output.

The `.prg` file is Lua 5.5 binary bytecode generated by the simulator's
`--compile` mode, using the same Lua build that loads programs on the OS.

- Hover documentation (a tooltip on the call under the mouse; parameter
  help covers the signature already).
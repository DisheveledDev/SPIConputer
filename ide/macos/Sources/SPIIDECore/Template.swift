import Foundation

/// File contents used when creating projects and components.
public enum Template {
    // MARK: New project

    public static func headerComponent(projectName: String) -> String {
        """
        -- \(projectName)
        -- Created with the SPIComputer IDE.
        --
        -- This file is a build-only snippet: it is concatenated into the
        -- generated program exactly as written. Keep constants, aliases and
        -- documentation here.
        --
        -- Program contract (see lua.md in the OS repo):
        --   setup()      runs once when the program starts
        --   tick()       runs repeatedly
        --   finish()     runs when the program exits
        --   on_keypress(key, shift, ctrl, cbm, restore)  optional
        --   on_control(index, up, down, left, right, fire) optional
        --
        -- Components build top-to-bottom into one Lua chunk, so locals
        -- declared in an earlier component are visible to later ones.

        """
    }

    public static func mainComponent(projectName: String) -> String {
        """
        -- main.lua — startup and shutdown.
        --
        -- setup() runs once when the program starts, before the first
        -- tick(); finish() runs once when the program exits. The loop
        -- lives in tick.lua and the input callbacks in input.lua. Every
        -- component ends up in one Lua chunk, so locals declared here are
        -- visible to the rest of the program.

        -- Choose a screen mode in setup() (switching clears the screen):
        --   ScreenMode(0)   -- 40x30 tiles, B&W
        --   ScreenMode(1)   -- 40x30 tiles, per-cell invert + 7 colours
        --   ScreenMode(2)   -- 80x60 tiles, B&W
        --   ScreenMode(3)   -- 80x60 tiles, per-cell invert + 7 colours
        --   ScreenMode(10)  -- 320x240 direct pixels, 256-entry palette
        --
        -- Write text with ScreenOut(x, y, char [, attr]) in tile modes.
        -- x and y are 0-based; char is an ASCII-aligned tile index:
        --   ScreenOut(0, 0, 65)          -- 'A' at the top-left corner
        --   ScreenOut(1, 0, 66, 0x07)    -- 'B' in colour 7
        --   ScreenClear(32)              -- fill the screen with spaces
        -- Attribute byte: bit 7 inverts, bits 0-2 select colour c, which
        -- uses palette entry c + 1. Mode 10 has no text: draw with
        -- ScreenPlot(x, y, colour). print() writes to the console only.

        function setup()
            -- Applies tiles/palettes/sounds from asset components. No-op
            -- when the project has none; delete it if you define assets
            -- in code.
            if ApplyAssets then ApplyAssets() end
        end

        -- finish() runs when the program exits, whether by ExitProgram()
        -- or an error. Video, audio and timers are cleaned up for you.

        function finish()
        end

        """
    }

    public static func tickComponent(projectName: String) -> String {
        """
        -- tick.lua — the main loop.
        --
        -- tick() is called repeatedly, as fast as possible, from after
        -- setup() returns until the program exits. It takes no arguments
        -- and no delta time; use TimeNow() for elapsed milliseconds. Keep
        -- it short so the program stays responsive.

        -- Input callbacks (input.lua) run just before each tick, so the
        -- latest state is always available here. InputControl(1) returns
        -- the live joystick state as a table with boolean fields up,
        -- down, left, right and fire.

        function tick()
        end

        """
    }

    /// Input handling: the two OS input callbacks.
    public static func inputComponent(projectName: String) -> String {
        """
        -- input.lua — input callbacks. Both are optional; delete either
        -- one. The OS calls them as events arrive, before the next tick,
        -- never during setup() or finish().
        --
        -- Events stay queued either way, so InputPoll() in tick() can
        -- still read key releases, raw edges and modifier-key events.

        -- on_keypress runs for key-down events.
        --   key     printable keys carry their ASCII code (Shift already
        --           applied); 13 Return, 8 Backspace, 27 Escape, 1-26
        --           Ctrl+letter; keys without an ASCII code use 128-131
        --           for the cursor keys, 132-138 for F1-F7, 139 Home and
        --           140 Run/Stop.
        --   shift   true while Shift is held
        --   ctrl    true while Ctrl is held
        --   cbm     true while the Commodore key is held
        --   restore true while Restore is held
        function on_keypress(key, shift, ctrl, cbm, restore)
        end

        -- on_control runs for every joystick change, with the full stick
        -- state after the event.
        --   index   the port: 0 = joystick 1, 1 = joystick 2
        --           (InputControl(n) instead takes 1 or 2)
        --   up, down, left, right, fire   booleans for the new state
        function on_control(index, up, down, left, right, fire)
        end

        """
    }

    // MARK: Added components

    public static func luaComponentStub(kind: ComponentKind, name: String) -> String {
        switch kind {
        case .snippet:
            """
            -- \(name) — build-only snippet.
            -- Concatenated into the generated program in build order.

            """
        default:
            """
            -- \(name).lua — runtime Lua.
            -- Concatenated into the generated program in build order; the
            -- program body runs once and should define setup()/tick()/finish()
            -- (or helper functions used by them). Components share one Lua
            -- chunk, so earlier locals are visible here.

            """
        }
    }

    // MARK: Simulator

    /// The `os.lua` written into the simulator's SD card so the built
    /// program starts immediately.
    public static func launcherOsLua(programFileName: String) -> String {
        let escaped = programFileName.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        return """
        -- os.lua — generated by the SPIComputer IDE to run a project
        -- directly in the simulator. Edit the project in the IDE instead.

        function setup()
            local ok, err = Launch("\(escaped)")
            if not ok then print("launch failed: " .. tostring(err)) end
        end

        function tick() end

        """
    }
}

// MARK: - Starter assets for newly added components

extension TilesAsset {
    /// A small example: two palette entries and one blank editable tile.
    public init(example: Bool) {
        self.init(
            palette: [
                RGB(r: 0, g: 0, b: 0),
                RGB(r: 255, g: 255, b: 255),
                RGB(r: 170, g: 255, b: 238),
            ],
            tiles: [TileDef(index: 128)])
    }
}

extension AudioAsset {
    /// A small example: a short square-wave blip and a one-note score.
    public init(example: Bool) {
        self.init(
            sounds: [
                SoundDef(id: 0, wave: .square, duty: 8, attack: 0, decay: 0,
                         sustain: 255, release: 30, volume: 255),
            ],
            scores: [
                ScoreDef(
                    name: "theme",
                    loop: true,
                    channels: [
                        [
                            NoteEvent(at: 0, sound: 0, note: "C4", dur: 150),
                            NoteEvent(at: 250, sound: 0, note: "E4", dur: 150),
                            NoteEvent(at: 500, sound: 0, note: "G4", dur: 150),
                        ],
                    ]),
            ])
    }
}

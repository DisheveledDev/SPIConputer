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
        --   setup()   runs once when the program starts
        --   tick()    runs repeatedly; poll input here with InputPoll()
        --   finish()  runs when the program exits
        --

        """
    }

    public static func mainComponent(projectName: String) -> String {
        """
        -- main.lua — program entry points.
        --
        -- The OS calls setup() once, then tick() as fast as possible, then
        -- finish() on exit. There is no separate input callback: drain the
        -- event queue with InputPoll() at the top of tick().

        local blink = false
        local last_key = ""
        local ticks = 0
        local shown_fire = false
        local spin = 0
        local SPINNER = { 45, 47, 124, 92 }      -- - / | \

        local function put(x, y, char, attr)
            ScreenOut(x, y, char, attr or 0)
        end

        local function put_string(x, y, text, attr)
            for i = 1, #text do
                put(x + i - 1, y, text:byte(i), attr)
            end
        end

        function setup()
            -- Apply tiles/palettes/sounds from asset components. This is a
            -- no-op when the project has none; delete it if you define your
            -- assets directly in code.
            if ApplyAssets then ApplyAssets() end

            ScreenMode(1)                -- 40x30 tiles, per-cell colour
            ScreenClear(32)              -- space (ScreenClear takes a tile code)
            put_string(0, 0, "\(projectName)", 0x02)
            put_string(0, 2, "Ctrl+Q quits", 0x07)

            -- A 500 ms timer blinks the marker at the top-left corner.
            -- Timers pause with the program and die with it.
            TimerCreate(function()
                blink = not blink
                put(0, 0, blink and 42 or 32)      -- '*' / ' '
            end, 500)
        end

        function tick()
            ticks = ticks + 1

            -- 1. Input: drain pending key and joystick events.
            local changed = false
            while true do
                local ev = InputPoll()
                if not ev then break end
                if ev.type == "key" and ev.pressed == 1 then
                    if ev.key == 17 then                    -- Ctrl+Q
                        ExitProgram()
                        return
                    elseif ev.key >= 32 and ev.key < 128 then
                        last_key = string.char(ev.key)
                        changed = true
                    end
                end
            end

            -- 2. Joystick state (up/down/left/right/fire booleans).
            local joy = InputControl(1)
            local firing = joy and joy.fire or false

            -- 3. Status line. Redraw only when something changed: formatting
            --    a string every tick would churn the program's 64 KB heap.
            if changed or firing ~= shown_fire then
                shown_fire = firing
                local status = string.format("key %-3s  fire %-3s  ticks %d",
                    last_key == "" and "-" or last_key,
                    firing and "yes" or "no", ticks)
                put_string(2, 4, status, 0x05)
            end

            -- 4. A tiny spinner proves ticks are running (no allocation).
            if ticks % 30 == 0 then
                spin = spin % 4 + 1
                put(0, 3, SPINNER[spin])
            end
        end

        function finish()
            -- Video, audio and timers are cleaned up automatically.
            print("\(projectName): finished after " .. ticks .. " ticks")
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
            -- (or helper functions used by them).

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

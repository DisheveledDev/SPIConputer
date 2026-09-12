import Foundation

/// A location in a component source file (1-based line).
public struct SourceLocation: Sendable, Equatable, Hashable {
    public let componentID: ComponentRef.ID
    public let line: Int

    public init(componentID: ComponentRef.ID, line: Int) {
        self.componentID = componentID
        self.line = line
    }
}

/// Maps generated program lines back to their source component.
public struct LineMap: Sendable, Equatable {
    /// One emitted block: `count` generated lines starting at `start`.
    /// `component` is the component line of the block's first line, or nil
    /// for generated code (headers, asset wrappers).
    public struct Span: Sendable, Equatable {
        public let start: Int
        public let count: Int
        public let component: SourceLocation?

        public init(start: Int, count: Int, component: SourceLocation?) {
            self.start = start
            self.count = count
            self.component = component
        }
    }

    public let spans: [Span]

    public init(spans: [Span]) {
        self.spans = spans
    }

    /// Source location for a 1-based generated line, or nil for generated
    /// code. A component location points at the matching line of that
    /// component's file.
    public func source(forGeneratedLine line: Int) -> SourceLocation? {
        for span in spans where line >= span.start && line < span.start + span.count {
            guard let component = span.component else { return nil }
            return SourceLocation(
                componentID: component.componentID,
                line: component.line + (line - span.start))
        }
        return nil
    }
}

/// The result of building a project.
public struct BuildProduct: Sendable {
    public let lua: String
    public let outputURL: URL
    public let componentCount: Int
    public let lineMap: LineMap
}

public enum BuildError: Error, LocalizedError, Equatable {
    case noComponents
    case missingComponentFile(String)
    case unreadableComponent(String)

    public var errorDescription: String? {
        switch self {
        case .noComponents:
            "The project has no components. Add one with the + menu."
        case .missingComponentFile(let file):
            "Component file not found: \(file)"
        case .unreadableComponent(let file):
            "Component file is not valid UTF-8: \(file)"
        }
    }
}

/// Compiles a project's components into one Lua program.
///
/// Emission rules:
/// - `lua` / `snippet` components are copied verbatim, in manifest order.
/// - `tiles` / `audio` components emit a `__asset_*` function and
///   register it in `__spi_assets`.
/// - A generated `ApplyAssets()` (called from the starter `setup()`)
///   runs every registered asset function, so assets are applied inside
///   setup() where the program's video/audio state is current.
public enum ProjectBuilder {
    public static func render(_ project: Project, timestamp: Date = Date()) throws -> String {
        try renderProduct(project, timestamp: timestamp).lua
    }

    /// Render plus the generated-to-component line map used for
    /// compile-error attribution.
    public static func renderProduct(
        _ project: Project, timestamp: Date = Date()
    ) throws -> (lua: String, lineMap: LineMap) {
        guard !project.manifest.components.isEmpty else {
            throw BuildError.noComponents
        }

        var emitter = Emitter()
        emitter.append(header(project: project, timestamp: timestamp), component: nil)
        if !project.manifest.interactive {
            emitter.append("__spi_interactive = false\n\n", component: nil)
        }

        var assetNames: [String] = []
        for component in project.manifest.components {
            emitter.append(
                "\n-- ==== component: \(component.name) (\(component.kind.rawValue)) ====\n\n",
                component: nil)
            let url = project.fileURL(for: component)
            guard FileManager.default.fileExists(atPath: url.path) else {
                throw BuildError.missingComponentFile(component.file)
            }
            guard let data = try? Data(contentsOf: url),
                  let text = String(data: data, encoding: .utf8)
            else {
                throw BuildError.unreadableComponent(component.file)
            }
            switch component.kind {
            case .lua, .snippet:
                var body = text
                if !body.hasSuffix("\n") {
                    body += "\n"
                }
                emitter.append(
                    body, component: SourceLocation(componentID: component.id, line: 1))
            case .tiles:
                guard let asset = try? JSONCoding.decode(TilesAsset.self, from: data) else {
                    throw BuildError.unreadableComponent(component.file)
                }
                let name = uniqueAssetName(from: component.name, used: &assetNames)
                emitter.append(emit(tiles: asset, functionName: name), component: nil)
            case .audio:
                guard let asset = try? JSONCoding.decode(AudioAsset.self, from: data) else {
                    throw BuildError.unreadableComponent(component.file)
                }
                let name = uniqueAssetName(from: component.name, used: &assetNames)
                emitter.append(emit(audio: asset, functionName: name), component: nil)
            }
        }

        emitter.append("\n-- ==== generated: asset helper ====\n\n", component: nil)
        emitter.append(
            """
            -- Runs every asset component (tiles, palettes, sounds, scores).
            -- Called from setup(); safe to call more than once.
            function ApplyAssets()
                if __spi_assets then
                    for _, apply in ipairs(__spi_assets) do
                        apply()
                    end
                end
            end

            """,
            component: nil)
        return (emitter.text, LineMap(spans: emitter.spans))
    }

    @discardableResult
    public static func build(
        _ project: Project,
        timestamp: Date = Date(),
        writeToDisk: Bool = true
    ) throws -> BuildProduct {
        let product = try renderProduct(project, timestamp: timestamp)
        let outputURL = project.buildProductURL
        if writeToDisk {
            try FileManager.default.createDirectory(
                at: project.outputDirectoryURL, withIntermediateDirectories: true)
            try Data(product.lua.utf8).write(to: outputURL, options: .atomic)
        }
        return BuildProduct(
            lua: product.lua,
            outputURL: outputURL,
            componentCount: project.manifest.components.count,
            lineMap: product.lineMap)
    }

    /// Accumulates emitted text while tracking generated line numbers.
    private struct Emitter {
        var text = ""
        var spans: [LineMap.Span] = []
        private var nextLine = 1

        mutating func append(_ body: String, component: SourceLocation?) {
            guard !body.isEmpty else { return }
            let start = nextLine
            text += body
            let newlines = body.reduce(0) { $0 + ($1 == "\n" ? 1 : 0) }
            let lines = newlines + (body.hasSuffix("\n") ? 0 : 1)
            spans.append(LineMap.Span(start: start, count: lines, component: component))
            nextLine += lines
        }
    }

    // MARK: Emission

    private static func header(project: Project, timestamp: Date) -> String {
        let formatter = ISO8601DateFormatter()
        let lines = project.manifest.components
            .map { "--   \($0.kind.rawValue.padding(toLength: 7, withPad: " ", startingAt: 0)) \($0.name)" }
            .joined(separator: "\n")
        return """
        -- ================================================================
        -- \(project.manifest.name)
        -- Generated by the SPIComputer IDE — do not edit this file.
        -- Built \(formatter.string(from: timestamp)) from components:
        \(lines)
        -- ================================================================

        """
    }

    private static func emit(tiles asset: TilesAsset, functionName: String) -> String {
        var body = ""
        if let palette = asset.palette, !palette.isEmpty {
            let entries = palette
                .map { "\(clamp($0.r, 0, 255)), \(clamp($0.g, 0, 255)), \(clamp($0.b, 0, 255))" }
                .joined(separator: " }, { ")
            body += "    ScreenPaletteSet({ { \(entries) } })\n"
        }
        for tile in asset.tiles {
            let normalized = tile.normalized()
            let rows = normalized.rows.map(String.init).joined(separator: ", ")
            body += "    ScreenDefineTile(\(normalized.index), { \(rows) })\n"
        }
        if body.isEmpty {
            body = "    -- (no palette or tiles defined)\n"
        }
        return """
        local function \(functionName)()
        \(body)end
        __spi_assets = __spi_assets or {}
        table.insert(__spi_assets, \(functionName))

        """
    }

    private static func emit(audio asset: AudioAsset, functionName: String) -> String {
        var body = ""
        for sound in asset.sounds {
            body += """
                SoundDefine(\(sound.id), { wave = "\(sound.wave.rawValue)", duty = \(clamp(sound.duty, 1, 15)), attack = \(clamp(sound.attack, 0, 255)), decay = \(clamp(sound.decay, 0, 255)), sustain = \(clamp(sound.sustain, 0, 255)), release = \(clamp(sound.release, 0, 255)), volume = \(clamp(sound.volume, 0, 255)) })

            """
        }
        for score in asset.scores {
            var channelTexts: [String] = []
            for channel in score.channels {
                let events = channel.map { event in
                    "{at = \(max(0, event.at)), sound = \(clamp(event.sound, 0, 255)), note = \(noteLiteral(event.note)), dur = \(max(0, event.dur)), vol = \(clamp(event.vol, 0, 255)), pan = \(clamp(event.pan, -64, 63))}"
                }
                channelTexts.append("{ " + events.joined(separator: ", ") + " }")
            }
            let channels = channelTexts.isEmpty
                ? "{}"
                : "{\n            " + channelTexts.joined(separator: ",\n            ") + ",\n        }"
            let loop = score.loop ? ", loop = true" : ""
            body += """
                MusicDefine("\(escapeLua(score.name))", {channels = \(channels)\(loop)})

            """
        }
        if body.isEmpty {
            body = "    -- (no sounds or scores defined)\n"
        }
        return """
        local function \(functionName)()
        \(body)end
        __spi_assets = __spi_assets or {}
        table.insert(__spi_assets, \(functionName))

        """
    }

    // MARK: Helpers

    private static func uniqueAssetName(from componentName: String, used: inout [String]) -> String {
        let stem = componentName.map { character in
            character.isLetter || character.isNumber || character == "_" ? character : "_"
        }
        let name = "__asset_" + String(stem)
        var candidate = name
        var counter = 2
        while used.contains(candidate) {
            candidate = "\(name)_\(counter)"
            counter += 1
        }
        used.append(candidate)
        return candidate
    }

    private static func clamp(_ value: Int, _ low: Int, _ high: Int) -> Int {
        min(max(value, low), high)
    }

    private static func noteLiteral(_ note: String) -> String {
        if Int(note) != nil {
            return note
        }
        return "\"\(escapeLua(note))\""
    }

    private static func escapeLua(_ text: String) -> String {
        text.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
    }
}

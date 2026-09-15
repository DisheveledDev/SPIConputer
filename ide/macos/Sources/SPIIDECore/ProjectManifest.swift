import Foundation

/// What a project builds into, and how the OS runs it.
public enum ProjectKind: String, Codable, CaseIterable, Sendable {
    /// A `.prg` (and `.lua`) with nothing bundled: booted directly, or
    /// installed loose (system programs such as boot and the shell).
    case raw
    /// An `.app` bundle the shell launches on top of itself.
    case application
    /// A `.util` bundle: a command with no screen or sound of its own
    /// that runs once with arguments and returns a table to the shell.
    case utility
    /// A `.game` bundle: replaces the shell, owns the machine, and the
    /// device restarts when it exits.
    case game

    public var displayName: String {
        switch self {
        case .raw: "Raw program (.prg)"
        case .application: "Application (.app)"
        case .utility: "Command-line utility (.util)"
        case .game: "Game (.game)"
        }
    }

    public var summary: String {
        switch self {
        case .raw: "A bare .prg and .lua; runs directly with no OS behind it."
        case .application: "Launched from the shell's APPS list or by name; returns to the shell."
        case .utility: "Runs once with arguments, no screen of its own, returns a table to the shell. Installs to utils/."
        case .game: "Takes over the machine: the shell is freed and the device restarts when it exits. Installs to games/."
        }
    }

    /// Bundle folder extension, or nil for a raw program.
    public var bundleExtension: String? {
        switch self {
        case .raw: nil
        case .application: "app"
        case .utility: "util"
        case .game: "game"
        }
    }

    /// Where an install puts the product on the card.
    public var installDirectory: String {
        switch self {
        case .raw: "data"
        case .application: "apps"
        case .utility: "utils"
        case .game: "games"
        }
    }

    /// Whether the program has its own screen and sound (everything but
    /// a utility).
    public var interactive: Bool { self != .utility }

    /// The IDE's Run: boot the product itself, or install it into a card
    /// with the OS and boot the OS.
    public var runsUnderOS: Bool { self == .application || self == .utility }
}

/// The project manifest (`project.spiproj`): an ordered list of
/// components that build into one Lua program file.
public struct ProjectManifest: Codable, Sendable, Equatable {
    public static let currentFormatVersion = 2

    public var formatVersion: Int
    public var name: String
    public var kind: ProjectKind
    /// Card folder the product installs to, overriding the kind's default
    /// (`core` for the boot program and the shell).
    public var installDirectory: String?
    public var requiresVideo: Bool
    public var requiresAudio: Bool
    public var version: String
    /// One line shown by the shell's APPS picker next to the name.
    public var description: String
    public var iconFile: String?
    /// Frameworks (SDKLibrary ids such as "screen") injected, read-only,
    /// ahead of the components; unused functions are stripped at build.
    public var sdks: [String]
    /// Compile the .prg without debug info (line numbers, local names):
    /// about a fifth less heap once loaded, but runtime errors carry no
    /// line numbers. For resident system programs such as the shell.
    public var stripDebug: Bool
    public var components: [ComponentRef]

    /// Utilities have no screen or sound of their own.
    public var interactive: Bool { kind.interactive }

    public init(
        formatVersion: Int = ProjectManifest.currentFormatVersion,
        name: String,
        kind: ProjectKind = .application,
        installDirectory: String? = nil,
        requiresVideo: Bool = true,
        requiresAudio: Bool = true,
        version: String = "1.0",
        description: String = "",
        iconFile: String? = nil,
        sdks: [String] = [],
        stripDebug: Bool = false,
        components: [ComponentRef] = []
    ) {
        self.formatVersion = formatVersion
        self.name = name
        self.kind = kind
        self.installDirectory = installDirectory
        self.requiresVideo = requiresVideo
        self.requiresAudio = requiresAudio
        self.version = version
        self.description = description
        self.iconFile = iconFile
        self.sdks = sdks
        self.stripDebug = stripDebug
        self.components = components
    }

    enum CodingKeys: String, CodingKey {
        case formatVersion = "format_version"
        case name
        case kind
        case installDirectory = "install_directory"
        case requiresVideo = "requires_video"
        case requiresAudio = "requires_audio"
        case version
        case description
        case iconFile = "icon_file"
        case sdks
        case stripDebug = "strip_debug"
        case components
        // Format 1 keys, read for migration only.
        case legacyInteractive = "interactive"
        case legacyOutputKind = "output_kind"
        case legacyOutputDirectory = "output_directory"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        formatVersion = try c.decodeIfPresent(Int.self, forKey: .formatVersion)
            ?? ProjectManifest.currentFormatVersion
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? "Untitled"
        if let kind = try c.decodeIfPresent(ProjectKind.self, forKey: .kind) {
            self.kind = kind
        } else {
            // Format 1: `output_kind: prg` was a system program, a
            // non-interactive project a utility, anything else an app.
            let interactive = try c.decodeIfPresent(Bool.self, forKey: .legacyInteractive) ?? true
            let outputKind = try c.decodeIfPresent(String.self, forKey: .legacyOutputKind)
            if outputKind == "prg" {
                kind = .raw
            } else {
                kind = interactive ? .application : .utility
            }
        }
        installDirectory = try c.decodeIfPresent(String.self, forKey: .installDirectory)
        if installDirectory == nil, try c.decodeIfPresent(String.self, forKey: .legacyOutputDirectory) == "../core" {
            installDirectory = "core" // format 1 system programs built into core/
        }
        let interactive = kind.interactive
        requiresVideo = try c.decodeIfPresent(Bool.self, forKey: .requiresVideo) ?? interactive
        requiresAudio = try c.decodeIfPresent(Bool.self, forKey: .requiresAudio) ?? interactive
        version = try c.decodeIfPresent(String.self, forKey: .version) ?? "1.0"
        description = try c.decodeIfPresent(String.self, forKey: .description) ?? ""
        iconFile = try c.decodeIfPresent(String.self, forKey: .iconFile)
        // No key: every framework (stripping keeps unused ones free), so
        // projects from before frameworks existed get them; an explicit
        // empty list opts out.
        sdks = try c.decodeIfPresent([String].self, forKey: .sdks)
            ?? SDKLibrary.available.map(\.id)
        stripDebug = try c.decodeIfPresent(Bool.self, forKey: .stripDebug) ?? false
        components = try c.decodeIfPresent([ComponentRef].self, forKey: .components) ?? []
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(ProjectManifest.currentFormatVersion, forKey: .formatVersion)
        try c.encode(name, forKey: .name)
        try c.encode(kind, forKey: .kind)
        try c.encodeIfPresent(installDirectory, forKey: .installDirectory)
        try c.encode(requiresVideo, forKey: .requiresVideo)
        try c.encode(requiresAudio, forKey: .requiresAudio)
        try c.encode(version, forKey: .version)
        try c.encode(description, forKey: .description)
        try c.encodeIfPresent(iconFile, forKey: .iconFile)
        try c.encode(sdks, forKey: .sdks)
        if stripDebug { try c.encode(true, forKey: .stripDebug) }
        try c.encode(components, forKey: .components)
    }
}

/// One component entry in the manifest. `file` is relative to the
/// project folder; `id` is stable across renames and reorders.
public struct ComponentRef: Codable, Sendable, Equatable, Identifiable {
    public var id: UUID
    public var name: String
    public var kind: ComponentKind
    public var file: String

    public init(id: UUID = UUID(), name: String, kind: ComponentKind, file: String) {
        self.id = id
        self.name = name
        self.kind = kind
        self.file = file
    }

    enum CodingKeys: String, CodingKey {
        case id, name, kind, file
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? "component"
        kind = try c.decodeIfPresent(ComponentKind.self, forKey: .kind) ?? .lua
        file = try c.decodeIfPresent(String.self, forKey: .file) ?? ""
    }
}

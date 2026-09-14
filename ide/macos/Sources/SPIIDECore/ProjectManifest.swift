import Foundation

public enum ProjectOutputKind: String, Codable, CaseIterable, Sendable {
    case app
    case prg

    public var displayName: String {
        switch self {
        case .app: "Application bundle (.app)"
        case .prg: "System program (.prg)"
        }
    }
}

/// The project manifest (`project.spiproj`): an ordered list of
/// components that build into one Lua program file.
public struct ProjectManifest: Codable, Sendable, Equatable {
    public static let currentFormatVersion = 1

    public var formatVersion: Int
    public var name: String
    /// Folder (relative to the project) the built program is written to.
    public var outputDirectory: String
    public var interactive: Bool
    public var outputKind: ProjectOutputKind
    public var requiresVideo: Bool
    public var requiresAudio: Bool
    public var version: String
    /// One line shown by the shell's APPS picker next to the name.
    public var description: String
    public var iconFile: String?
    public var components: [ComponentRef]

    public init(
        formatVersion: Int = ProjectManifest.currentFormatVersion,
        name: String,
        outputDirectory: String = "build",
        interactive: Bool = true,
        outputKind: ProjectOutputKind = .app,
        requiresVideo: Bool = true,
        requiresAudio: Bool = true,
        version: String = "1.0",
        description: String = "",
        iconFile: String? = nil,
        components: [ComponentRef] = []
    ) {
        self.formatVersion = formatVersion
        self.name = name
        self.outputDirectory = outputDirectory
        self.interactive = interactive
        self.outputKind = outputKind
        self.requiresVideo = requiresVideo
        self.requiresAudio = requiresAudio
        self.version = version
        self.description = description
        self.iconFile = iconFile
        self.components = components
    }

    enum CodingKeys: String, CodingKey {
        case formatVersion = "format_version"
        case name
        case outputDirectory = "output_directory"
        case interactive
        case outputKind = "output_kind"
        case requiresVideo = "requires_video"
        case requiresAudio = "requires_audio"
        case version
        case description
        case iconFile = "icon_file"
        case components
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        formatVersion = try c.decodeIfPresent(Int.self, forKey: .formatVersion)
            ?? ProjectManifest.currentFormatVersion
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? "Untitled"
        outputDirectory = try c.decodeIfPresent(String.self, forKey: .outputDirectory)
            ?? "build"
        interactive = try c.decodeIfPresent(Bool.self, forKey: .interactive) ?? true
        outputKind = try c.decodeIfPresent(ProjectOutputKind.self, forKey: .outputKind) ?? .app
        requiresVideo = try c.decodeIfPresent(Bool.self, forKey: .requiresVideo) ?? interactive
        requiresAudio = try c.decodeIfPresent(Bool.self, forKey: .requiresAudio) ?? interactive
        version = try c.decodeIfPresent(String.self, forKey: .version) ?? "1.0"
        description = try c.decodeIfPresent(String.self, forKey: .description) ?? ""
        iconFile = try c.decodeIfPresent(String.self, forKey: .iconFile)
        components = try c.decodeIfPresent([ComponentRef].self, forKey: .components) ?? []
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

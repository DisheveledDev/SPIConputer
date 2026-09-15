import Foundation

public struct AppMetadata: Codable, Sendable, Equatable {
    public let name: String
    public let version: String
    public let description: String
    public let type: String
    public let interactive: Bool
    public let video: Bool
    public let audio: Bool
    public let entry: String
    public let icon: String?

    public init(project: Project) {
        name = project.manifest.name
        version = project.manifest.version
        description = project.manifest.description
        type = project.manifest.kind.rawValue
        interactive = project.manifest.interactive
        video = project.manifest.requiresVideo
        audio = project.manifest.requiresAudio
        entry = "app.prg"
        icon = project.manifest.iconFile
    }
}

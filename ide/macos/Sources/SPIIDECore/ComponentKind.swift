import Foundation

/// The kinds of component a project can contain.
///
/// - `lua` and `snippet` are plain Lua files emitted verbatim, in build
///   order. Use `snippet` for build-only material (comments, constants).
/// - `tiles` is a JSON tile/palette asset compiled to `ScreenPaletteSet`
///   and `ScreenDefineTile` calls.
/// - `audio` is a JSON instrument/score asset compiled to `SoundDefine`
///   and `MusicDefine` calls.
public enum ComponentKind: String, Codable, CaseIterable, Sendable {
    case lua
    case snippet
    case tiles
    case audio

    public var displayName: String {
        switch self {
        case .lua: "Lua Code"
        case .snippet: "Snippet"
        case .tiles: "Tiles & Palette"
        case .audio: "Sounds & Music"
        }
    }

    public var symbolName: String {
        switch self {
        case .lua: "curlybraces"
        case .snippet: "text.alignleft"
        case .tiles: "squareshape.split.3x3"
        case .audio: "music.note"
        }
    }

    /// File extension used for components of this kind.
    public var fileExtension: String {
        switch self {
        case .lua, .snippet: "lua"
        case .tiles, .audio: "json"
        }
    }
}

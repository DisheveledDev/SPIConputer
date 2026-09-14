import Foundation

/// One completion list entry: the text inserted on accept plus the
/// expected parameters when the name is a known function.
public struct CompletionItem: Equatable, Sendable {
    public let name: String
    /// `(x, y, [attr])` for known functions, nil for other names.
    public let detail: String?

    public init(name: String, detail: String?) {
        self.name = name
        self.detail = detail
    }
}

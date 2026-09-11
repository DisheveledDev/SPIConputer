import Foundation

/// A compile-time diagnostic reported by the Lua compiler for the
/// generated program, resolved back to its source component.
public struct CompileDiagnostic: Sendable, Equatable {
    /// Message as reported by Lua (without the `[string ...]:line:` prefix).
    public let message: String
    /// 1-based line in the generated program (nil when Lua gave no line).
    public let generatedLine: Int?
    /// The component file and line this maps to (nil for generated code).
    public let location: SourceLocation?

    public init(message: String, generatedLine: Int?, location: SourceLocation?) {
        self.message = message
        self.generatedLine = generatedLine
        self.location = location
    }
}

/// Parses Lua loader errors of the form
/// `[string "name"]:12: message` (or `name:12: message`). Also extracts
/// the `at line N` context Lua adds for unclosed constructs, e.g.
/// `'end' expected (to close 'function' at line 41) near '<eof>'`.
public enum LuaErrorParser {
    public struct Parsed: Sendable, Equatable {
        public let line: Int?
        /// Line named inside the message text ("at line N"), if any.
        public let contextLine: Int?
        public let message: String

        public init(line: Int?, contextLine: Int?, message: String) {
            self.line = line
            self.contextLine = contextLine
            self.message = message
        }
    }

    public static func parse(_ raw: String) -> Parsed {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        let primary = firstMatch(#":(\d+):\s?"#, in: trimmed)
        var message = trimmed
        if let primary {
            let whole = primary.range
            let messageStart = trimmed.index(
                trimmed.startIndex, offsetBy: whole.location + whole.length)
            message = String(trimmed[messageStart...])
                .trimmingCharacters(in: .whitespacesAndNewlines)
        }
        if message.isEmpty {
            message = trimmed
        }
        var contextLine: Int?
        if let context = firstMatch(#"at line (\d+)"#, in: message),
           let range = Range(context.range(at: 1), in: message) {
            contextLine = Int(message[range])
        }
        return Parsed(
            line: primary.flatMap { match -> Int? in
                guard let range = Range(match.range(at: 1), in: trimmed) else { return nil }
                return Int(trimmed[range])
            },
            contextLine: contextLine,
            message: message)
    }

    private static func firstMatch(_ pattern: String, in text: String) -> NSTextCheckingResult? {
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return nil }
        return regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text))
    }
}

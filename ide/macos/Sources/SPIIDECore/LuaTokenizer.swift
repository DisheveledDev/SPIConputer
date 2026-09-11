import Foundation

/// Tokenizer used for syntax colouring in the code editor. Pure and
/// testable; the AppKit layer maps token kinds to colours.
public enum LuaTokenizer {
    public enum Kind: Sendable, Equatable {
        case comment
        case string
        case number
        case keyword
        case function
    }

    public struct Token: Sendable, Equatable {
        public let range: NSRange
        public let kind: Kind

        public init(range: NSRange, kind: Kind) {
            self.range = range
            self.kind = kind
        }
    }

    public static func tokenize(_ source: String) -> [Token] {
        let text = source as NSString
        let length = text.length
        var tokens: [Token] = []

        func isDigit(_ c: unichar) -> Bool { c >= 0x30 && c <= 0x39 }
        func isHexDigit(_ c: unichar) -> Bool {
            isDigit(c) || (c >= 0x41 && c <= 0x46) || (c >= 0x61 && c <= 0x66)
        }
        func isIdentStart(_ c: unichar) -> Bool {
            (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A) || c == 0x5F
        }
        func isIdent(_ c: unichar) -> Bool { isIdentStart(c) || isDigit(c) }

        func lineEnd(from index: Int) -> Int {
            var i = index
            while i < length && text.character(at: i) != 0x0A {
                i += 1
            }
            return i
        }

        /// End of a `[[ ... ]]` / `[=[ ... ]=]` block starting at `index`
        /// (which points at the first `[`), or nil when it never closes.
        func longBracketEnd(from index: Int) -> Int? {
            var i = index + 1
            var level = 0
            while i < length && text.character(at: i) == 0x3D { // '='
                level += 1
                i += 1
            }
            guard i < length, text.character(at: i) == 0x5B else { return nil } // '['
            i += 1
            while i < length {
                guard text.character(at: i) == 0x5D else { // ']'
                    i += 1
                    continue
                }
                var j = i + 1
                var seen = 0
                while j < length && text.character(at: j) == 0x3D {
                    seen += 1
                    j += 1
                }
                if seen == level, j < length, text.character(at: j) == 0x5D {
                    return j + 1
                }
                i += 1
            }
            return nil
        }

        var index = 0
        while index < length {
            let c = text.character(at: index)

            // Comments: `--` line or `--[[ ... ]]`.
            if c == 0x2D, index + 1 < length, text.character(at: index + 1) == 0x2D {
                if index + 2 < length, text.character(at: index + 2) == 0x5B,
                   let end = longBracketEnd(from: index + 2) {
                    tokens.append(Token(range: NSRange(location: index, length: end - index), kind: .comment))
                    index = end
                    continue
                }
                let end = lineEnd(from: index)
                tokens.append(Token(range: NSRange(location: index, length: end - index), kind: .comment))
                index = end
                continue
            }

            // Long strings.
            if c == 0x5B, index + 1 < length {
                let next = text.character(at: index + 1)
                if next == 0x5B || next == 0x3D, let end = longBracketEnd(from: index) {
                    tokens.append(Token(range: NSRange(location: index, length: end - index), kind: .string))
                    index = end
                    continue
                }
            }

            // Short strings.
            if c == 0x22 || c == 0x27 {
                var j = index + 1
                while j < length {
                    let inner = text.character(at: j)
                    if inner == 0x5C { // backslash
                        j += 2
                        continue
                    }
                    if inner == c {
                        j += 1
                        break
                    }
                    if inner == 0x0A {
                        break
                    }
                    j += 1
                }
                let end = min(j, length)
                tokens.append(Token(range: NSRange(location: index, length: end - index), kind: .string))
                index = end
                continue
            }

            // Numbers.
            if isDigit(c) {
                var j = index
                if c == 0x30, j + 1 < length,
                   text.character(at: j + 1) == 0x78 || text.character(at: j + 1) == 0x58 { // 0x
                    j += 2
                    while j < length && (isHexDigit(text.character(at: j)) || text.character(at: j) == 0x2E) {
                        j += 1
                    }
                } else {
                    while j < length, isDigit(text.character(at: j)) {
                        j += 1
                    }
                    if j < length, text.character(at: j) == 0x2E { // .
                        j += 1
                        while j < length, isDigit(text.character(at: j)) {
                            j += 1
                        }
                    }
                    if j < length, text.character(at: j) == 0x65 || text.character(at: j) == 0x45 { // e/E
                        var k = j + 1
                        if k < length, text.character(at: k) == 0x2B || text.character(at: k) == 0x2D {
                            k += 1
                        }
                        if k < length, isDigit(text.character(at: k)) {
                            j = k
                            while j < length, isDigit(text.character(at: j)) {
                                j += 1
                            }
                        }
                    }
                }
                tokens.append(Token(range: NSRange(location: index, length: j - index), kind: .number))
                index = j
                continue
            }

            // Identifiers (including one dotted member: `string.format`).
            if isIdentStart(c) {
                var j = index
                while j < length, isIdent(text.character(at: j)) {
                    j += 1
                }
                var word = text.substring(with: NSRange(location: index, length: j - index))
                var end = j
                if j < length, text.character(at: j) == 0x2E, j + 1 < length,
                   isIdentStart(text.character(at: j + 1)) {
                    var k = j + 1
                    while k < length, isIdent(text.character(at: k)) {
                        k += 1
                    }
                    word = text.substring(with: NSRange(location: index, length: k - index))
                    end = k
                }
                if keywords.contains(word) {
                    tokens.append(Token(range: NSRange(location: index, length: end - index), kind: .keyword))
                } else if knownFunctions.contains(word) {
                    tokens.append(Token(range: NSRange(location: index, length: end - index), kind: .function))
                }
                index = end
                continue
            }

            index += 1
        }
        return tokens
    }

    private static let keywords = Set(LuaCompletion.keywords)

    /// Known function names, including dotted members (`string.format`).
    private static let knownFunctions: Set<String> = {
        var names = Set(LuaCompletion.builtins + LuaCompletion.spiComputer)
        // `Print`-style call sites only carry the base name; keep the
        // member forms too so `string.format` colours as a whole.
        return names
    }()
}

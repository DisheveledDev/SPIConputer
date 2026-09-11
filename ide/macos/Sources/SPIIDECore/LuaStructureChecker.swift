import Foundation

/// Cheap Lua block-structure check used to attribute compile errors that
/// Lua reports at `<eof>` (a missing `end` makes the real culprit the
/// *opening* line, which the user needs highlighted).
///
/// It counts `function`/`if`/`for`/`while`/`repeat` against `end`/`until`
/// using the same tokenizer as syntax colouring, so keywords inside
/// strings and comments are ignored.
public enum LuaStructureChecker {
    public struct Issue: Sendable, Equatable {
        /// 1-based line of the unclosed opener (or the stray closer).
        public let line: Int
        public let message: String

        public init(line: Int, message: String) {
            self.line = line
            self.message = message
        }
    }

    private struct Opener {
        let keyword: String
        let line: Int
    }

    public static func check(_ source: String) -> Issue? {
        let text = source as NSString
        var stack: [Opener] = []
        var line = 1
        var lastEnd = 0

        for token in LuaTokenizer.tokenize(source) where token.kind == .keyword {
            line += newlines(in: text, from: lastEnd, to: token.range.location)
            lastEnd = token.range.location + token.range.length
            let keywordLine = line
            let keyword = text.substring(with: token.range)

            switch keyword {
            case "function", "if", "for", "while", "repeat":
                stack.append(Opener(keyword: keyword, line: keywordLine))
            case "do":
                // `for i = 1, 10 do` / `while x do` / `if x then`: the `do`
                // belongs to the opener already pushed for those forms.
                if let top = stack.last, ["if", "for", "while"].contains(top.keyword) {
                    continue
                }
                stack.append(Opener(keyword: "do", line: keywordLine))
            case "end":
                if stack.isEmpty {
                    return Issue(line: keywordLine, message: "unexpected 'end' (no block is open)")
                }
                let opener = stack.removeLast()
                if opener.keyword == "repeat" {
                    return Issue(
                        line: keywordLine,
                        message: "'end' found where 'until' was expected "
                            + "(repeat opened at line \(opener.line))")
                }
            case "until":
                if stack.isEmpty || stack.last?.keyword != "repeat" {
                    return Issue(line: keywordLine, message: "unexpected 'until' (no repeat is open)")
                }
                stack.removeLast()
            default:
                break
            }
        }

        if let unclosed = stack.last {
            let closer = unclosed.keyword == "repeat" ? "until" : "end"
            return Issue(
                line: unclosed.line,
                message: "missing '\(closer)' to close '\(unclosed.keyword)'")
        }
        return nil
    }

    private static func newlines(in text: NSString, from start: Int, to end: Int) -> Int {
        guard end > start else { return 0 }
        var count = 0
        for index in start..<min(end, text.length) where text.character(at: index) == 0x0A {
            count += 1
        }
        return count
    }
}

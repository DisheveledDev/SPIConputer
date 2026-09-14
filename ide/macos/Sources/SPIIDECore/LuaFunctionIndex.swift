import Foundation

/// Finds function definitions in Lua source so the editor can offer them
/// as completions and parameter help. Strings and comments are ignored
/// via the syntax tokenizer.
public enum LuaFunctionIndex {
    /// Named definitions in source order; a later definition of the same
    /// name replaces an earlier one, as Lua would at runtime.
    public static func functions(in source: String) -> [LuaSignature] {
        let text = LuaSignatureHelp.maskedText(source)
        var result: [LuaSignature] = []
        var search = 0
        while let keyword = nextWord("function", from: search, in: text) {
            search = keyword.location + keyword.length
            guard let definition = definition(keyword: keyword, in: text) else {
                continue
            }
            result.removeAll { $0.name == definition.name }
            result.append(LuaSignature(
                name: definition.name, parameters: definition.parameters))
        }
        return result
    }

    /// The signature of `name` as defined in `source`: an exact match
    /// first, then by method suffix so `f:read` at the call site finds
    /// `obj:read` (or `:read`) where it is defined.
    public static func signature(named name: String, in source: String) -> LuaSignature? {
        let functions = functions(in: source)
        if let exact = functions.last(where: { $0.name == name }) {
            return exact
        }
        guard name.hasPrefix(":") else { return nil }
        return functions.last { $0.name.hasSuffix(name) }
    }

    // MARK: Parsing

    /// Parses the definition whose `function` keyword sits at `keyword`:
    /// `function name(...)`, `local function name(...)` or the anonymous
    /// `name = function(...)` form. Returns nil when the header is
    /// incomplete (no parameter list) or anonymous.
    private static func definition(
        keyword: NSRange, in text: NSString
    ) -> (name: String, parameters: [String])? {
        let end = text.length
        var cursor = skipSpace(text, from: keyword.location + keyword.length, to: end)
        var name: String?
        if cursor < end, text.character(at: cursor) != 0x28 { // (
            guard let read = readName(text, from: cursor, to: end) else { return nil }
            name = read.name
            cursor = skipSpace(text, from: read.next, to: end)
        } else {
            name = assignmentName(before: keyword.location, in: text)
        }
        guard let name, !name.isEmpty,
              cursor < end, text.character(at: cursor) == 0x28, // (
              let close = closingParenthesis(from: cursor, in: text)
        else { return nil }
        let raw = text.substring(
            with: NSRange(location: cursor + 1, length: close - cursor - 1))
        let parameters = raw
            .split(separator: ",")
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        return (name, parameters)
    }

    /// Name for `x = function(...)` / `local x = function(...)`, read
    /// backwards from the keyword past the `=`.
    private static func assignmentName(before index: Int, in text: NSString) -> String? {
        var cursor = skipSpaceBackwards(text, from: index)
        guard cursor > 0, text.character(at: cursor - 1) == 0x3D else { return nil } // =
        cursor = skipSpaceBackwards(text, from: cursor - 1)
        var start = cursor
        while start > 0, isNameCharacter(text.character(at: start - 1)) {
            start -= 1
        }
        guard start < cursor else { return nil }
        return text.substring(with: NSRange(location: start, length: cursor - start))
    }

    /// Identifier immediately after `start` (`a`, `a.b`, `a:b`).
    private static func readName(
        _ text: NSString, from start: Int, to end: Int
    ) -> (name: String, next: Int)? {
        var index = start
        while index < end, isNameCharacter(text.character(at: index)) {
            index += 1
        }
        guard index > start else { return nil }
        return (text.substring(with: NSRange(location: start, length: index - start)), index)
    }

    /// The `)` closing the parameter list opened at `open`. Lua parameter
    /// lists cannot nest, so the first closer wins.
    private static func closingParenthesis(from open: Int, in text: NSString) -> Int? {
        var index = open + 1
        while index < text.length {
            if text.character(at: index) == 0x29 { return index } // )
            index += 1
        }
        return nil
    }

    private static func nextWord(_ word: String, from start: Int, in text: NSString) -> NSRange? {
        var search = max(start, 0)
        while search < text.length {
            let range = text.range(
                of: word, options: [],
                range: NSRange(location: search, length: text.length - search))
            guard range.location != NSNotFound else { return nil }
            let before = range.location == 0 ? nil : text.character(at: range.location - 1)
            let afterIndex = range.location + range.length
            let after = afterIndex >= text.length ? nil : text.character(at: afterIndex)
            if !isIdentifier(before), !isIdentifier(after) {
                return range
            }
            search = range.location + 1
        }
        return nil
    }

    private static func skipSpace(_ text: NSString, from start: Int, to end: Int) -> Int {
        var index = max(start, 0)
        while index < end, isSpace(text.character(at: index)) {
            index += 1
        }
        return index
    }

    private static func skipSpaceBackwards(_ text: NSString, from index: Int) -> Int {
        var cursor = min(max(index, 0), text.length)
        while cursor > 0, isSpace(text.character(at: cursor - 1)) {
            cursor -= 1
        }
        return cursor
    }

    private static func isIdentifier(_ c: unichar?) -> Bool {
        guard let c else { return false }
        return isIdentifier(c)
    }

    private static func isIdentifier(_ c: unichar) -> Bool {
        (c >= 0x41 && c <= 0x5A) || (c >= 0x61 && c <= 0x7A)
            || (c >= 0x30 && c <= 0x39) || c == 0x5F
    }

    /// Identifier characters plus the `.`/`:` used in dotted names.
    private static func isNameCharacter(_ c: unichar) -> Bool {
        isIdentifier(c) || c == 0x2E || c == 0x3A
    }

    private static func isSpace(_ c: unichar) -> Bool {
        c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D
    }
}

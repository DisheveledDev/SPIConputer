import Foundation

/// One framework function: a top-level `function Name.Sub(...)` ... `end`
/// block (or a `local function helper(...)` block) with the comment
/// lines above it.
public struct SDKBlock: Sendable, Equatable {
    public let name: String
    public let isLocal: Bool
    /// The block's full text including its comment lines, ending in "\n".
    public let text: String
    /// Signature from the `---` line, or from the definition itself.
    public let signature: LuaSignature
    /// The `--` lines that follow the `---` line, joined.
    public let summary: String
    /// Dotted and plain names the block's body mentions.
    public let references: Set<String>
}

/// A framework: a read-only Lua file the IDE injects into programs that
/// select it, with the unused functions stripped.
public struct SDK: Sendable, Equatable, Identifiable {
    /// File stem (`screen`, `overlay`, ...): what the manifest stores.
    public let id: String
    public let title: String
    public let summary: String
    public let namespaces: [String]
    /// Everything above the first block; always emitted.
    public let preamble: String
    /// Dotted names the preamble assigns at column 0 (`Screen.COLS`,
    /// `Attributes.Red`): completed as plain names, no parameter list.
    public let constants: [String]
    public let blocks: [SDKBlock]

    public var signatures: [LuaSignature] {
        blocks.filter { !$0.isLocal }.map(\.signature)
    }
}

/// Loads the bundled frameworks and strips them for a build.
///
/// File contract (see the header of any `Resources/sdk/*.lua`): the
/// preamble is everything before the first column-0 `function`; each
/// column-0 `function Name.Sub(` ... column-0 `end` is a block; a
/// column-0 `local function helper(` block is a local helper; the `---`
/// line directly above a block is its signature; `--` lines around it
/// are its description.
public enum SDKLibrary {
    /// Bundled frameworks in presentation order.
    public static let available: [SDK] = {
        let order = ["screen", "overlay", "text", "timer", "sound", "input"]
        var found: [String: SDK] = [:]
        if let urls = Bundle.module.urls(forResourcesWithExtension: "lua", subdirectory: "sdk") {
            for url in urls {
                let id = url.deletingPathExtension().lastPathComponent
                if let data = try? Data(contentsOf: url),
                   let text = String(data: data, encoding: .utf8) {
                    found[id] = parse(id: id, text: text)
                }
            }
        }
        var result: [SDK] = []
        for id in order {
            if let sdk = found.removeValue(forKey: id) { result.append(sdk) }
        }
        result += found.values.sorted { $0.id < $1.id }
        return result
    }()

    public static func sdk(id: String) -> SDK? {
        available.first { $0.id == id }
    }

    /// Signatures of every public function in the selected frameworks,
    /// for completion and parameter help.
    public static func signatures(for ids: [String]) -> [LuaSignature] {
        ids.compactMap(sdk(id:)).flatMap(\.signatures)
    }

    /// The constants the selected frameworks define, for completion.
    public static func constants(for ids: [String]) -> [String] {
        ids.compactMap(sdk(id:)).flatMap(\.constants)
    }

    // MARK: Parsing

    public static func parse(id: String, text: String) -> SDK {
        var title = id.prefix(1).uppercased() + id.dropFirst()
        var summary = ""
        var namespaces: [String] = []
        var preamble = ""
        var blocks: [SDKBlock] = []
        var pendingComments: [String] = []
        var block: (name: String, isLocal: Bool, comments: [String], body: [String])?

        for rawLine in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = String(rawLine)
            if var current = block {
                current.body.append(line)
                if line == "end" {
                    blocks.append(makeBlock(
                        name: current.name, isLocal: current.isLocal,
                        comments: current.comments, body: current.body))
                    block = nil
                } else {
                    block = current
                }
                continue
            }
            if let header = blockHeader(line) {
                block = (header.name, header.isLocal, pendingComments, [line])
                pendingComments = []
                continue
            }
            if line.hasPrefix("--") {
                if line.hasPrefix("-- SDK:") {
                    title = line.dropFirst("-- SDK:".count).trimmingCharacters(in: .whitespaces)
                } else if line.hasPrefix("-- Summary:") {
                    summary = line.dropFirst("-- Summary:".count).trimmingCharacters(in: .whitespaces)
                } else if line.hasPrefix("-- Namespaces:") {
                    namespaces = line.dropFirst("-- Namespaces:".count)
                        .split(separator: ",")
                        .map { $0.trimmingCharacters(in: .whitespaces) }
                }
                pendingComments.append(line)
                continue
            }
            // Any other line (code or blank) ends a run of comments: they
            // belong to the preamble, not to a later block.
            for comment in pendingComments { preamble += comment + "\n" }
            pendingComments = []
            preamble += line + "\n"
        }
        for comment in pendingComments { preamble += comment + "\n" }
        if let current = block { // unterminated: keep it as text
            blocks.append(makeBlock(
                name: current.name, isLocal: current.isLocal,
                comments: current.comments, body: current.body))
        }
        // The preamble ends where the first block's comments start; trim
        // trailing blank lines so the emitted file stays tidy.
        while preamble.hasSuffix("\n\n") { preamble.removeLast() }
        return SDK(id: id, title: title, summary: summary, namespaces: namespaces,
                   preamble: preamble, constants: constants(inPreamble: preamble),
                   blocks: blocks)
    }

    /// `Name.CONST = value` lines at column 0 of the preamble. Namespace
    /// tables (`Screen = Screen or {}`, `Input.Keyboard = ...`) are not
    /// constants.
    static func constants(inPreamble preamble: String) -> [String] {
        var result: [String] = []
        for rawLine in preamble.split(separator: "\n") {
            let line = String(rawLine)
            guard let first = line.first, first.isUppercase else { continue }
            var name = ""
            for c in line {
                if c.isLetter || c.isNumber || c == "_" || c == "." { name.append(c) } else { break }
            }
            guard name.contains("."), !name.hasSuffix(".") else { continue }
            let rest = line.dropFirst(name.count).trimmingCharacters(in: .whitespaces)
            guard rest.hasPrefix("=") else { continue }
            let value = rest.dropFirst().trimmingCharacters(in: .whitespaces)
            if value.hasPrefix(name + " or") { continue }
            result.append(name)
        }
        return result
    }

    private static func blockHeader(_ line: String) -> (name: String, isLocal: Bool)? {
        var rest = Substring(line)
        var isLocal = false
        if rest.hasPrefix("local function ") {
            isLocal = true
            rest = rest.dropFirst("local function ".count)
        } else if rest.hasPrefix("function ") {
            rest = rest.dropFirst("function ".count)
        } else {
            return nil
        }
        var name = ""
        for c in rest {
            if c.isLetter || c.isNumber || c == "_" || c == "." { name.append(c) } else { break }
        }
        guard !name.isEmpty, rest.dropFirst(name.count).trimmingCharacters(in: .whitespaces).hasPrefix("(")
        else { return nil }
        return (name, isLocal)
    }

    private static func makeBlock(
        name: String, isLocal: Bool, comments: [String], body: [String]
    ) -> SDKBlock {
        let text = (comments + body).joined(separator: "\n") + "\n"
        var signature: LuaSignature?
        var summaryLines: [String] = []
        for comment in comments {
            if comment.hasPrefix("---") {
                signature = parseSignature(String(comment.dropFirst(3)), fallbackName: name)
                summaryLines = []
            } else if signature != nil {
                summaryLines.append(comment.dropFirst(2).trimmingCharacters(in: .whitespaces))
            }
        }
        if signature == nil {
            signature = LuaFunctionIndex.functions(in: body.joined(separator: "\n")).first
                ?? LuaSignature(name: name, parameters: [])
        }
        let bodyText = body.dropFirst().joined(separator: "\n")
        return SDKBlock(
            name: name, isLocal: isLocal, text: text, signature: signature!,
            summary: summaryLines.joined(separator: " "),
            references: identifiers(in: bodyText))
    }

    /// `--- Name(x, y [, attr])` → parameters `x`, `y`, `[attr]`; nested
    /// `[, style [, attr]]` yields `[style]`, `[attr]`.
    static func parseSignature(_ line: String, fallbackName: String) -> LuaSignature {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard let open = trimmed.firstIndex(of: "("), let close = trimmed.lastIndex(of: ")") else {
            return LuaSignature(name: fallbackName, parameters: [])
        }
        let name = trimmed[..<open].trimmingCharacters(in: .whitespaces)
        // "x [, attr]" is written with the bracket before the comma; move
        // it after, so each comma-separated piece carries its own bracket.
        let inner = trimmed[trimmed.index(after: open)..<close]
            .replacingOccurrences(of: #"\s*\[\s*,"#, with: ", [", options: .regularExpression)
        let parameters = inner.split(separator: ",").compactMap { piece -> String? in
            let optional = piece.contains("[")
            let bare = piece.replacingOccurrences(of: "[", with: "")
                .replacingOccurrences(of: "]", with: "")
                .trimmingCharacters(in: .whitespaces)
            guard !bare.isEmpty else { return nil }
            return optional ? "[\(bare)]" : bare
        }
        return LuaSignature(name: name.isEmpty ? fallbackName : name, parameters: parameters)
    }

    /// Dotted and plain identifiers in `text`, with strings and comments
    /// masked out. `Screen.OutText` contributes "Screen.OutText" and
    /// "Screen"; a plain `helper` contributes "helper"; a method call
    /// `t:Pause(` contributes ":Pause", which keeps any block whose last
    /// name component is Pause (framework objects dispatch `obj:Method()`
    /// to `Namespace.Method`).
    static func identifiers(in text: String) -> Set<String> {
        let masked = LuaSignatureHelp.maskedText(text) as String
        var result = Set<String>()
        var current = ""
        var previousWasColon = false
        func flush() {
            if !current.isEmpty {
                result.insert(current)
                if let dot = current.firstIndex(of: ".") {
                    result.insert(String(current[..<dot]))
                }
                current = ""
            }
        }
        for c in masked {
            if c.isLetter || c.isNumber || c == "_" {
                if current.isEmpty && previousWasColon {
                    current = ":"
                }
                current.append(c)
                previousWasColon = false
            } else if c == "." && !current.isEmpty && !current.hasPrefix(":") {
                current.append(c)
                previousWasColon = false
            } else {
                flush()
                previousWasColon = (c == ":")
            }
        }
        flush()
        return result
    }

    /// Whether `used` (from identifiers(in:)) refers to the block `name`.
    static func references(_ used: Set<String>, block name: String) -> Bool {
        if used.contains(name) { return true }
        if let dot = name.lastIndex(of: ".") {
            return used.contains(":" + name[name.index(after: dot)...])
        }
        return false
    }

    // MARK: Stripping

    /// The framework text a program needs: the preamble plus every block
    /// the program's sources mention, plus what those blocks mention in
    /// turn. Nil when the program uses nothing from it.
    public static func emit(_ sdk: SDK, usedBy sources: [String]) -> String? {
        var used = Set<String>()
        for source in sources {
            used.formUnion(identifiers(in: source))
        }
        return emit(sdk, referenced: used)
    }

    public static func emit(_ sdk: SDK, referenced used: Set<String>) -> String? {
        let byName = Dictionary(sdk.blocks.map { ($0.name, $0) }, uniquingKeysWith: { a, _ in a })
        var kept = Set<String>()
        var queue = sdk.blocks.filter { !$0.isLocal && references(used, block: $0.name) }.map(\.name)
        while let name = queue.popLast() {
            guard kept.insert(name).inserted, let block = byName[name] else { continue }
            for other in sdk.blocks where !kept.contains(other.name) && references(block.references, block: other.name) {
                queue.append(other.name)
            }
        }
        // A program that only reads the constants still needs the preamble.
        guard !kept.isEmpty || sdk.constants.contains(where: used.contains) else { return nil }
        var text = sdk.preamble
        if !text.hasSuffix("\n") { text += "\n" }
        for block in sdk.blocks where kept.contains(block.name) {
            text += "\n" + block.text
        }
        return text
    }
}

import Foundation

/// One documented name: a function, method, namespace, module, constant
/// or keyword. See Resources/help/README.md for the schema.
public struct HelpEntry: Codable, Sendable, Equatable, Identifiable {
    public struct Parameter: Codable, Sendable, Equatable {
        public var name: String
        public var type: String
        public var optional: Bool
        public var description: String

        public init(name: String, type: String = "", optional: Bool = false, description: String = "") {
            self.name = name
            self.type = type
            self.optional = optional
            self.description = description
        }

        enum CodingKeys: String, CodingKey { case name, type, optional, description }

        public init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            name = try c.decode(String.self, forKey: .name)
            type = try c.decodeIfPresent(String.self, forKey: .type) ?? ""
            optional = try c.decodeIfPresent(Bool.self, forKey: .optional) ?? false
            description = try c.decodeIfPresent(String.self, forKey: .description) ?? ""
        }
    }

    public var name: String
    public var kind: String
    public var group: String
    public var framework: String?
    public var signature: String
    public var summary: String
    public var description: String
    public var parameters: [Parameter]
    public var returns: String
    public var example: String
    public var seeAlso: [String]
    /// The document the entry came from (`lua`, `os`, `sdk`); set on load.
    public var source: String = ""

    public var id: String { name }

    enum CodingKeys: String, CodingKey {
        case name, kind, group, framework, signature, summary, description, parameters, returns, example, seeAlso
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        kind = try c.decodeIfPresent(String.self, forKey: .kind) ?? "function"
        group = try c.decodeIfPresent(String.self, forKey: .group) ?? ""
        framework = try c.decodeIfPresent(String.self, forKey: .framework)
        signature = try c.decodeIfPresent(String.self, forKey: .signature) ?? name
        summary = try c.decodeIfPresent(String.self, forKey: .summary) ?? ""
        description = try c.decodeIfPresent(String.self, forKey: .description) ?? ""
        parameters = try c.decodeIfPresent([Parameter].self, forKey: .parameters) ?? []
        returns = try c.decodeIfPresent(String.self, forKey: .returns) ?? ""
        example = try c.decodeIfPresent(String.self, forKey: .example) ?? ""
        seeAlso = try c.decodeIfPresent([String].self, forKey: .seeAlso) ?? []
    }

    public init(
        name: String, kind: String = "function", group: String = "", framework: String? = nil,
        signature: String? = nil, summary: String = "", description: String = "",
        parameters: [Parameter] = [], returns: String = "", example: String = "",
        seeAlso: [String] = [], source: String = ""
    ) {
        self.name = name
        self.kind = kind
        self.group = group
        self.framework = framework
        self.signature = signature ?? name
        self.summary = summary
        self.description = description
        self.parameters = parameters
        self.returns = returns
        self.example = example
        self.seeAlso = seeAlso
        self.source = source
    }

    /// The namespace part of a dotted name ("Screen" for "Screen.OutText",
    /// "Input.Keyboard" for "Input.Keyboard.Callback"), or nil.
    public var namespace: String? {
        guard let dot = name.lastIndex(of: ".") else { return nil }
        return String(name[..<dot])
    }
}

/// One help file: a source label and its entries.
public struct HelpDocument: Codable, Sendable {
    public var source: String
    public var title: String
    public var entries: [HelpEntry]
}

/// Every help entry the IDE ships, loaded from Resources/help/*.json.
public enum HelpLibrary {
    public static let documents: [HelpDocument] = {
        guard let urls = Bundle.module.urls(forResourcesWithExtension: "json", subdirectory: "help") else {
            return []
        }
        var loaded: [HelpDocument] = []
        for url in urls.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            if let data = try? Data(contentsOf: url),
               var document = try? JSONDecoder().decode(HelpDocument.self, from: data) {
                for index in document.entries.indices {
                    document.entries[index].source = document.source
                }
                loaded.append(document)
            }
        }
        return loaded
    }()

    public static let entries: [HelpEntry] = documents.flatMap(\.entries)

    private static let byName: [String: HelpEntry] = {
        var map: [String: HelpEntry] = [:]
        for entry in entries where map[entry.name] == nil {
            map[entry.name] = entry
        }
        return map
    }()

    /// The entry for a name as written at a call site: exact, then a
    /// method (`f:read` or `:read` finds ":read"; `t:Pause` finds the
    /// framework function whose last component is Pause).
    public static func entry(named rawName: String) -> HelpEntry? {
        let name = rawName.trimmingCharacters(in: .whitespaces)
        if let exact = byName[name] { return exact }
        if let colon = name.lastIndex(of: ":") {
            let method = String(name[name.index(after: colon)...])
            if let exact = byName[":" + method] { return exact }
            return entries.first { $0.kind == "function" && $0.namespace != nil && $0.name.hasSuffix("." + method) }
        }
        return nil
    }

    /// Entries directly inside a namespace or module, functions first.
    public static func members(of namespace: String) -> [HelpEntry] {
        entries
            .filter { $0.namespace == namespace }
            .sorted {
                if ($0.kind == "constant") != ($1.kind == "constant") { return $0.kind != "constant" }
                return $0.name < $1.name
            }
    }

    /// Entries whose name, summary or group contains `query`
    /// (case-insensitive), name matches first.
    public static func search(_ query: String, limit: Int = 40) -> [HelpEntry] {
        let q = query.trimmingCharacters(in: .whitespaces).lowercased()
        guard !q.isEmpty else { return [] }
        let byNamePrefix = entries.filter { $0.name.lowercased().hasPrefix(q) }
        let byNameContains = entries.filter { !$0.name.lowercased().hasPrefix(q) && $0.name.lowercased().contains(q) }
        let byText = entries.filter {
            !$0.name.lowercased().contains(q)
                && ($0.summary.lowercased().contains(q) || $0.group.lowercased().contains(q))
        }
        return Array((byNamePrefix + byNameContains + byText).prefix(limit))
    }

    /// Group headings in file order, with their entries (for browsing).
    public static var groups: [(title: String, entries: [HelpEntry])] {
        var order: [String] = []
        var map: [String: [HelpEntry]] = [:]
        for entry in entries {
            if map[entry.group] == nil { order.append(entry.group) }
            map[entry.group, default: []].append(entry)
        }
        return order.map { ($0, map[$0] ?? []) }
    }
}

import Foundation

/// A loaded project: its folder on disk plus the parsed manifest.
public struct Project: Sendable, Equatable {
    public static let manifestName = "project.spiproj"

    public var root: URL
    public var manifest: ProjectManifest

    public init(root: URL, manifest: ProjectManifest) {
        self.root = root
        self.manifest = manifest
    }

    public var manifestURL: URL {
        root.appendingPathComponent(Self.manifestName)
    }

    public func fileURL(for component: ComponentRef) -> URL {
        root.appendingPathComponent(component.file)
    }

    /// File name stem of the built program (safe for FAT/SD card paths).
    public var programFileStem: String {
        let sanitized = manifest.name
            .map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? $0 : "-" }
        let joined = String(sanitized).trimmingCharacters(in: CharacterSet(charactersIn: "-"))
        return joined.isEmpty ? "program" : joined
    }

    public var programFileName: String {
        "\(programFileStem).lua"
    }

    public var prgFileName: String {
        "\(programFileStem).prg"
    }

    /// Every product goes under the project's own `build/` folder.
    public var buildDirectoryURL: URL {
        root.appendingPathComponent("build")
    }

    public var buildProductURL: URL {
        buildDirectoryURL.appendingPathComponent(programFileName)
    }

    public var prgProductURL: URL {
        buildDirectoryURL.appendingPathComponent(prgFileName)
    }

    /// The bundle folder (`name.app`, `name.util`, `name.game`), or nil
    /// for a raw program.
    public var bundleURL: URL? {
        guard let ext = manifest.kind.bundleExtension else { return nil }
        return buildDirectoryURL.appendingPathComponent("\(programFileStem).\(ext)")
    }

    /// Name of the bundle folder as installed on the card.
    public var bundleName: String? {
        manifest.kind.bundleExtension.map { "\(programFileStem).\($0)" }
    }

    public var bundleProgramURL: URL? {
        bundleURL?.appendingPathComponent("app.prg")
    }

    public var bundleMetadataURL: URL? {
        bundleURL?.appendingPathComponent("app.json")
    }

    /// Card folder the product installs to.
    public var installDirectory: String {
        manifest.installDirectory ?? manifest.kind.installDirectory
    }
}

/// Disk operations for projects and components.
public enum ProjectStore {
    // MARK: Load / save

    public static func load(from root: URL) throws -> Project {
        let manifestURL = root.appendingPathComponent(Project.manifestName)
        let data = try Data(contentsOf: manifestURL)
        let manifest = try JSONCoding.decode(ProjectManifest.self, from: data)
        return Project(root: root, manifest: manifest)
    }

    public static func save(_ project: Project) throws {
        let data = try JSONCoding.encode(project.manifest)
        try data.write(to: project.manifestURL, options: .atomic)
    }

    // MARK: New project

    /// Creates a new project folder with the full starter template.
    @discardableResult
    public static func createProject(
        named name: String, in parent: URL, kind: ProjectKind = .application
    ) throws -> Project {
        let root = parent.appendingPathComponent(name)
        let fm = FileManager.default
        try fm.createDirectory(at: root, withIntermediateDirectories: true)

        // New projects get every framework: unused functions are stripped
        // at build, so selecting them all costs nothing.
        var manifest = ProjectManifest(
            name: name, kind: kind, sdks: SDKLibrary.available.map(\.id))
        try writeTemplate(to: root, manifest: &manifest)
        let project = Project(root: root, manifest: manifest)
        try save(project)
        return project
    }

    /// Writes the "full" starter template (header + main + input + tick)
    /// and returns the manifest refs for it.
    private static func writeTemplate(to root: URL, manifest: inout ProjectManifest) throws {
        let componentsDir = root.appendingPathComponent("components")
        try FileManager.default.createDirectory(at: componentsDir, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(
            at: root.appendingPathComponent("resources"), withIntermediateDirectories: true)

        let header = Template.headerComponent(projectName: manifest.name)
        let main = Template.mainComponent(
            projectName: manifest.name, kind: manifest.kind)
        let input = Template.inputComponent(projectName: manifest.name)
        let tick = Template.tickComponent(projectName: manifest.name)

        try writeText(header, to: componentsDir.appendingPathComponent("00-header.lua"))
        try writeText(main, to: componentsDir.appendingPathComponent("main.lua"))
        try writeText(input, to: componentsDir.appendingPathComponent("input.lua"))
        try writeText(tick, to: componentsDir.appendingPathComponent("tick.lua"))

        manifest.components = [
            ComponentRef(name: "header", kind: .snippet, file: "components/00-header.lua"),
            ComponentRef(name: "main", kind: .lua, file: "components/main.lua"),
            ComponentRef(name: "input", kind: .lua, file: "components/input.lua"),
            ComponentRef(name: "tick", kind: .lua, file: "components/tick.lua"),
        ]
    }

    // MARK: Component files

    /// Creates the file for a new component and returns its manifest ref.
    public static func addComponent(
        kind: ComponentKind,
        named name: String,
        to project: inout Project
    ) throws -> ComponentRef {
        let stem = uniqueFileStem(base: name, ext: kind.fileExtension, in: project)
        let fileName = "\(stem).\(kind.fileExtension)"
        let relative = "components/\(fileName)"
        let url = project.root.appendingPathComponent(relative)
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true)

        let content: String
        switch kind {
        case .lua, .snippet:
            content = Template.luaComponentStub(kind: kind, name: name)
        case .tiles:
            content = try JSONCoding.encode(TilesAsset(example: true))
                .asUTF8String() + "\n"
        case .audio:
            content = try JSONCoding.encode(AudioAsset(example: true))
                .asUTF8String() + "\n"
        }
        try writeText(content, to: url)

        let ref = ComponentRef(name: name, kind: kind, file: relative)
        return ref
    }

    public static func removeComponent(_ component: ComponentRef, from project: inout Project) {
        let url = project.fileURL(for: component)
        try? FileManager.default.removeItem(at: url)
        project.manifest.components.removeAll { $0.id == component.id }
    }

    public static func moveComponents(in project: inout Project, from offsets: IndexSet, to destination: Int) {
        var items = project.manifest.components
        let moving = offsets.sorted().map { items[$0] }
        for index in offsets.sorted(by: >) {
            items.remove(at: index)
        }
        let removedBeforeDestination = offsets.filter { $0 < destination }.count
        let insertAt = max(0, min(destination - removedBeforeDestination, items.count))
        items.insert(contentsOf: moving, at: insertAt)
        project.manifest.components = items
    }

    // MARK: Component content

    public static func readText(_ component: ComponentRef, in project: Project) throws -> String {
        let data = try Data(contentsOf: project.fileURL(for: component))
        return String(data: data, encoding: .utf8) ?? ""
    }

    public static func writeText(_ text: String, to url: URL) throws {
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(text.utf8).write(to: url, options: .atomic)
    }

    public static func readTiles(_ component: ComponentRef, in project: Project) throws -> TilesAsset {
        let data = try Data(contentsOf: project.fileURL(for: component))
        return try JSONCoding.decode(TilesAsset.self, from: data)
    }

    public static func writeTiles(_ asset: TilesAsset, for component: ComponentRef, in project: Project) throws {
        let data = try JSONCoding.encode(asset)
        try (data.asUTF8String() + "\n").write(
            to: project.fileURL(for: component), atomically: true, encoding: .utf8)
    }

    public static func readAudio(_ component: ComponentRef, in project: Project) throws -> AudioAsset {
        let data = try Data(contentsOf: project.fileURL(for: component))
        return try JSONCoding.decode(AudioAsset.self, from: data)
    }

    public static func writeAudio(_ asset: AudioAsset, for component: ComponentRef, in project: Project) throws {
        let data = try JSONCoding.encode(asset)
        try (data.asUTF8String() + "\n").write(
            to: project.fileURL(for: component), atomically: true, encoding: .utf8)
    }

    // MARK: Helpers

    private static func uniqueFileStem(base: String, ext: String, in project: Project) -> String {
        let stem = base
            .map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? $0 : "-" }
            .reduce(into: "") { $0.append($1) }
        let used = Set(project.manifest.components.map { ($0.file as NSString).lastPathComponent })
        let cleanStem = stem.isEmpty ? "component" : stem
        var candidate = "\(cleanStem).\(ext)"
        var counter = 2
        while used.contains(candidate) {
            candidate = "\(cleanStem)-\(counter).\(ext)"
            counter += 1
        }
        return (candidate as NSString).deletingPathExtension
    }
}

extension Data {
    public func asUTF8String() -> String {
        String(data: self, encoding: .utf8) ?? ""
    }
}

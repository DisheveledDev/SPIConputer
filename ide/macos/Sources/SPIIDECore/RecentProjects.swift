import Foundation

/// One entry in the recently opened list.
public struct RecentProject: Codable, Sendable, Equatable, Identifiable {
    public let path: String
    public let name: String
    public let lastOpened: Date

    public var id: String { path }
    public var url: URL { URL(fileURLWithPath: path, isDirectory: true) }

    public init(path: String, name: String, lastOpened: Date) {
        self.path = path
        self.name = name
        self.lastOpened = lastOpened
    }

    /// The path with the home folder abbreviated, for display.
    public var displayPath: String {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        if path.hasPrefix(home) {
            return "~" + path.dropFirst(home.count)
        }
        return path
    }
}

/// Recently opened projects, most recent first, kept in UserDefaults.
/// Opening a project moves it to the front; the list is capped, and
/// `prune()` drops projects whose folder has gone.
public final class RecentProjectsStore {
    public static let defaultKey = "recentProjects"
    public static let defaultLimit = 10

    private let defaults: UserDefaults
    private let key: String
    private let limit: Int

    public init(
        defaults: UserDefaults = .standard,
        key: String = RecentProjectsStore.defaultKey,
        limit: Int = RecentProjectsStore.defaultLimit
    ) {
        self.defaults = defaults
        self.key = key
        self.limit = max(1, limit)
    }

    public var entries: [RecentProject] {
        guard let data = defaults.data(forKey: key),
              let decoded = try? JSONCoding.decode([RecentProject].self, from: data)
        else { return [] }
        return decoded
    }

    /// Records a project as just opened (front of the list, one entry
    /// per folder).
    public func record(_ project: Project, at date: Date = Date()) {
        record(root: project.root, name: project.manifest.name, at: date)
    }

    public func record(root: URL, name: String, at date: Date = Date()) {
        let path = root.standardizedFileURL.path
        var list = entries.filter { $0.path != path }
        list.insert(RecentProject(path: path, name: name, lastOpened: date), at: 0)
        if list.count > limit {
            list.removeLast(list.count - limit)
        }
        save(list)
    }

    public func remove(path: String) {
        save(entries.filter { $0.path != path })
    }

    public func clear() {
        defaults.removeObject(forKey: key)
    }

    /// Drops entries whose project folder (or manifest) no longer exists.
    /// Returns the surviving list.
    @discardableResult
    public func prune(fileManager: FileManager = .default) -> [RecentProject] {
        let kept = entries.filter { entry in
            fileManager.fileExists(
                atPath: entry.url.appendingPathComponent(Project.manifestName).path)
        }
        if kept.count != entries.count {
            save(kept)
        }
        return kept
    }

    private func save(_ list: [RecentProject]) {
        if let data = try? JSONCoding.encode(list) {
            defaults.set(data, forKey: key)
        }
    }
}

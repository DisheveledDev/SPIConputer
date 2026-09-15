import Foundation
import Testing

@testable import SPIIDECore

@Suite("Recent projects")
struct RecentProjectsTests {
    private func makeStore(limit: Int = 10) -> (RecentProjectsStore, UserDefaults) {
        let suite = "spiide-recents-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defaults.removePersistentDomain(forName: suite)
        return (RecentProjectsStore(defaults: defaults, limit: limit), defaults)
    }

    private func makeProjectFolder() throws -> URL {
        let parent = FileManager.default.temporaryDirectory
            .appendingPathComponent("spiide-recents-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
        return try ProjectStore.createProject(named: "Recent", in: parent).root
    }

    @Test func recordsMostRecentFirstWithoutDuplicates() {
        let (store, _) = makeStore()
        let a = URL(fileURLWithPath: "/tmp/a", isDirectory: true)
        let b = URL(fileURLWithPath: "/tmp/b", isDirectory: true)
        store.record(root: a, name: "A", at: Date(timeIntervalSince1970: 1))
        store.record(root: b, name: "B", at: Date(timeIntervalSince1970: 2))
        #expect(store.entries.map(\.name) == ["B", "A"])

        // Opening A again moves it to the front with the new date.
        store.record(root: a, name: "A renamed", at: Date(timeIntervalSince1970: 3))
        #expect(store.entries.map(\.name) == ["A renamed", "B"])
        #expect(store.entries.count == 2)
        #expect(store.entries[0].lastOpened == Date(timeIntervalSince1970: 3))
    }

    @Test func capsTheListAtTheLimit() {
        let (store, _) = makeStore(limit: 3)
        for i in 1...5 {
            store.record(root: URL(fileURLWithPath: "/tmp/p\(i)", isDirectory: true),
                         name: "P\(i)", at: Date(timeIntervalSince1970: TimeInterval(i)))
        }
        #expect(store.entries.map(\.name) == ["P5", "P4", "P3"])
    }

    @Test func removeAndClear() {
        let (store, defaults) = makeStore()
        store.record(root: URL(fileURLWithPath: "/tmp/a", isDirectory: true), name: "A")
        store.record(root: URL(fileURLWithPath: "/tmp/b", isDirectory: true), name: "B")
        store.remove(path: "/tmp/a")
        #expect(store.entries.map(\.name) == ["B"])
        store.clear()
        #expect(store.entries.isEmpty)
        #expect(defaults.data(forKey: RecentProjectsStore.defaultKey) == nil)
    }

    @Test func pruneDropsProjectsWhoseFolderIsGone() throws {
        let (store, _) = makeStore()
        let real = try makeProjectFolder()
        defer { try? FileManager.default.removeItem(at: real.deletingLastPathComponent()) }
        store.record(root: real, name: "Real")
        store.record(root: URL(fileURLWithPath: "/tmp/spiide-missing-\(UUID())", isDirectory: true),
                     name: "Gone")
        let kept = store.prune()
        #expect(kept.map(\.name) == ["Real"])
        #expect(store.entries.map(\.name) == ["Real"])
    }

    @Test func survivesAReload() {
        let (store, defaults) = makeStore()
        store.record(root: URL(fileURLWithPath: "/tmp/a", isDirectory: true), name: "A")
        let again = RecentProjectsStore(defaults: defaults)
        #expect(again.entries.map(\.name) == ["A"])
    }

    @Test func displayPathAbbreviatesHome() {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let entry = RecentProject(path: home + "/src/game", name: "game", lastOpened: Date())
        #expect(entry.displayPath == "~/src/game")
        let other = RecentProject(path: "/Volumes/card/game", name: "game", lastOpened: Date())
        #expect(other.displayPath == "/Volumes/card/game")
    }
}

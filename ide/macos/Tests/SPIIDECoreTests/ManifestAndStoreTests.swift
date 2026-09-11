import Foundation
import Testing

@testable import SPIIDECore

@Suite("Manifest and store")
struct ManifestAndStoreTests {
    private func makeTempParent() throws -> URL {
        let parent = FileManager.default.temporaryDirectory
            .appendingPathComponent("spiide-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
        return parent
    }

    @Test func manifestRoundTrip() throws {
        let manifest = ProjectManifest(
            name: "Demo",
            outputDirectory: "build",
            components: [
                ComponentRef(name: "header", kind: .snippet, file: "components/00-header.lua"),
                ComponentRef(name: "music", kind: .audio, file: "components/music.json"),
            ])
        let data = try JSONCoding.encode(manifest)
        let decoded = try JSONCoding.decode(ProjectManifest.self, from: data)
        #expect(decoded == manifest)
    }

    @Test func manifestDefaultsForMissingKeys() throws {
        let json = Data(#"{"name": "Tiny"}"#.utf8)
        let manifest = try JSONCoding.decode(ProjectManifest.self, from: json)
        #expect(manifest.formatVersion == ProjectManifest.currentFormatVersion)
        #expect(manifest.outputDirectory == "build")
        #expect(manifest.components.isEmpty)
    }

    @Test func componentDefaultsForMissingKeys() throws {
        let json = Data(#"{"name": "main"}"#.utf8)
        let component = try JSONCoding.decode(ComponentRef.self, from: json)
        #expect(component.kind == .lua)
        #expect(component.file == "")
    }

    @Test func createProjectWritesTemplate() throws {
        let parent = try makeTempParent()
        defer { try? FileManager.default.removeItem(at: parent) }

        let project = try ProjectStore.createProject(named: "My Game", in: parent)

        let mainURL = project.root.appendingPathComponent("components/main.lua")
        let inputURL = project.root.appendingPathComponent("components/input.lua")
        let tickURL = project.root.appendingPathComponent("components/tick.lua")
        let headerURL = project.root.appendingPathComponent("components/00-header.lua")
        #expect(FileManager.default.fileExists(atPath: mainURL.path))
        #expect(FileManager.default.fileExists(atPath: inputURL.path))
        #expect(FileManager.default.fileExists(atPath: tickURL.path))
        #expect(FileManager.default.fileExists(atPath: headerURL.path))
        #expect(FileManager.default.fileExists(atPath: project.manifestURL.path))
        #expect(project.manifest.components.count == 4)
        #expect(project.manifest.components[0].kind == .snippet)
        #expect(project.manifest.components[1].kind == .lua)
        #expect(project.manifest.components[2].kind == .lua)
        #expect(project.manifest.components[3].kind == .lua)
        #expect(project.manifest.components.map(\.name) == ["header", "main", "input", "tick"])
        #expect(project.programFileName == "My-Game.lua")

        let main = try String(contentsOf: mainURL, encoding: .utf8)
        #expect(main.contains("function main()"))
        #expect(main.contains("function setup()"))
        #expect(main.contains("function finish()"))
        #expect(main.contains("ApplyAssets"))

        let input = try String(contentsOf: inputURL, encoding: .utf8)
        #expect(input.contains("function on_keypress(key, shift, ctrl, cbm, restore)"))
        #expect(input.contains("function on_control(index, up, down, left, right, fire)"))
        #expect(input.contains("ExitProgram()"))

        let tick = try String(contentsOf: tickURL, encoding: .utf8)
        #expect(tick.contains("function tick()"))
        #expect(tick.contains("InputControl(1)"))
        #expect(!tick.contains("InputPoll("))

        let reloaded = try ProjectStore.load(from: project.root)
        #expect(reloaded == project)
    }

    @Test func addComponentUsesUniqueNamesAndWritesFiles() throws {
        let parent = try makeTempParent()
        defer { try? FileManager.default.removeItem(at: parent) }
        var project = try ProjectStore.createProject(named: "Demo", in: parent)

        let first = try ProjectStore.addComponent(kind: .tiles, named: "sprites", to: &project)
        project.manifest.components.append(first)
        let second = try ProjectStore.addComponent(kind: .tiles, named: "sprites", to: &project)
        project.manifest.components.append(second)

        #expect(first.file == "components/sprites.json")
        #expect(second.file == "components/sprites-2.json")
        #expect(FileManager.default.fileExists(atPath: project.fileURL(for: second).path))
        #expect(try ProjectStore.readTiles(first, in: project).tiles.count == 1)

        ProjectStore.removeComponent(first, from: &project)
        #expect(project.manifest.components.count == 5)
        #expect(!FileManager.default.fileExists(atPath: project.fileURL(for: first).path))
    }
}

import Foundation
import Testing

import SPIIDECore

@testable import SPIIDE

@Suite("App model diagnostics")
@MainActor
struct AppModelDiagnosticsTests {
    @Test func diagnosticPrefersLiveCheckThenMergedCheck() {
        let model = AppModel()
        let first = ComponentRef(name: "main", kind: .lua, file: "components/main.lua")
        let second = ComponentRef(name: "tick", kind: .lua, file: "components/tick.lua")
        model.project = Project(
            root: URL(fileURLWithPath: "/tmp/spiide-diagnostic-test"),
            manifest: ProjectManifest(name: "Test", components: [first, second]))

        #expect(model.diagnostic(for: first.id) == nil)

        // The merged-program check attributes its error to a component.
        model.compileDiagnostic = CompileDiagnostic(
            message: "merged error", generatedLine: 4,
            location: SourceLocation(componentID: first.id, line: 3))
        #expect(model.diagnostic(for: first.id)?.message == "merged error")
        #expect(model.diagnostic(for: second.id) == nil)

        // A live per-file result wins over the merged one.
        model.componentDiagnostics[first.id] = CompileDiagnostic(
            message: "live error", generatedLine: nil,
            location: SourceLocation(componentID: first.id, line: 1))
        #expect(model.diagnostic(for: first.id)?.message == "live error")
    }

    @Test func projectFunctionsCoverAllComponents() {
        let model = AppModel()
        let parent = FileManager.default.temporaryDirectory
            .appendingPathComponent("spiide-functions-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: parent) }

        model.createProject(named: "Functions", in: parent)
        let names = Set(model.projectFunctions.map(\.name))
        #expect(names.isSuperset(of: [
            "setup", "finish", "on_keypress", "on_control", "tick",
        ]))
    }
}

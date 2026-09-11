import AppKit
import Foundation
import Observation

import SPIIDECore

/// Application state: the open project, the editor buffers, the build
/// console and the running simulator process.
@MainActor
@Observable
final class AppModel {
    // Project
    var project: Project?
    var selectedComponentID: ComponentRef.ID?
    var showingNewProject = false
    var showingOpenPanel = false
    var errorMessage: String?
    var renameTargetID: ComponentRef.ID?

    // Editor buffers (loaded for the selected component)
    var luaText = ""
    var tilesAsset = TilesAsset()
    var audioAsset = AudioAsset()

    // Console / run state
    var console = ""
    var lastBuildURL: URL?
    var isRunning = false

    // Simulator
    var simulatorURL: URL?
    var simulatorPathPreference: String = UserDefaults.standard.string(
        forKey: AppModel.simulatorPathKey) ?? "" {
        didSet {
            UserDefaults.standard.set(simulatorPathPreference, forKey: Self.simulatorPathKey)
            refreshSimulator()
        }
    }

    private static let simulatorPathKey = "simulatorPath"
    private let consoleStream: AsyncStream<String>
    private let consoleContinuation: AsyncStream<String>.Continuation
    private var process: Process?
    private var saveTask: Task<Void, Never>?

    init() {
        (consoleStream, consoleContinuation) = AsyncStream<String>.makeStream()
        Task { [weak self] in
            guard let self else { return }
            for await chunk in consoleStream {
                appendConsole(chunk)
            }
        }
        refreshSimulator()
    }

    // MARK: Selection

    var selectedComponent: ComponentRef? {
        guard let project, let selectedComponentID else { return nil }
        return project.manifest.components.first { $0.id == selectedComponentID }
    }

    func componentSelectionChanged() {
        saveNow()
        loadSelectedComponent()
    }

    private func loadSelectedComponent() {
        guard let project, let component = selectedComponent else { return }
        do {
            switch component.kind {
            case .lua, .snippet:
                luaText = try ProjectStore.readText(component, in: project)
            case .tiles:
                tilesAsset = try ProjectStore.readTiles(component, in: project)
            case .audio:
                audioAsset = try ProjectStore.readAudio(component, in: project)
            }
        } catch {
            errorMessage = "Cannot read \(component.file): \(error.localizedDescription)"
        }
    }

    // MARK: Editing / saving

    /// Debounced save of the selected component's editor buffer.
    func scheduleSave() {
        guard let component = selectedComponent, let project else { return }
        saveTask?.cancel()
        saveTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled, let self,
                  self.selectedComponentID == component.id
            else { return }
            self.save(component: component, in: project)
        }
    }

    func saveNow() {
        saveTask?.cancel()
        guard let component = selectedComponent, let project else { return }
        save(component: component, in: project)
    }

    private func save(component: ComponentRef, in project: Project) {
        do {
            switch component.kind {
            case .lua, .snippet:
                try ProjectStore.writeText(luaText, to: project.fileURL(for: component))
            case .tiles:
                try ProjectStore.writeTiles(tilesAsset, for: component, in: project)
            case .audio:
                try ProjectStore.writeAudio(audioAsset, for: component, in: project)
            }
        } catch {
            errorMessage = "Cannot save \(component.file): \(error.localizedDescription)"
        }
    }

    // MARK: Project management

    func createProject(named name: String, in parent: URL) {
        do {
            let created = try ProjectStore.createProject(named: name, in: parent)
            open(created)
            appendConsole("Created \(created.root.path)\n")
        } catch {
            errorMessage = "Cannot create project: \(error.localizedDescription)"
        }
    }

    func openProject(at root: URL) {
        do {
            open(try ProjectStore.load(from: root))
            appendConsole("Opened \(root.path)\n")
        } catch {
            errorMessage = "Cannot open project: \(error.localizedDescription)"
        }
    }

    private func open(_ project: Project) {
        self.project = project
        selectedComponentID = project.manifest.components.first?.id
        loadSelectedComponent()
        lastBuildURL = project.buildProductURL
    }

    func addComponent(kind: ComponentKind) {
        guard var project else { return }
        do {
            let name = defaultComponentName(for: kind, in: project)
            let ref = try ProjectStore.addComponent(kind: kind, named: name, to: &project)
            project.manifest.components.append(ref)
            try ProjectStore.save(project)
            self.project = project
            selectedComponentID = ref.id
            loadSelectedComponent()
        } catch {
            errorMessage = "Cannot add component: \(error.localizedDescription)"
        }
    }

    func removeComponent(id: ComponentRef.ID) {
        guard var project,
              let component = project.manifest.components.first(where: { $0.id == id })
        else { return }
        ProjectStore.removeComponent(component, from: &project)
        try? ProjectStore.save(project)
        self.project = project
        if selectedComponentID == id {
            selectedComponentID = project.manifest.components.first?.id
            loadSelectedComponent()
        }
    }

    func renameComponent(id: ComponentRef.ID, to newName: String) {
        guard var project,
              let index = project.manifest.components.firstIndex(where: { $0.id == id })
        else { return }
        let trimmed = newName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        project.manifest.components[index].name = trimmed
        try? ProjectStore.save(project)
        self.project = project
    }

    func moveComponents(from offsets: IndexSet, to destination: Int) {
        guard var project else { return }
        ProjectStore.moveComponents(in: &project, from: offsets, to: destination)
        try? ProjectStore.save(project)
        self.project = project
    }

    private func defaultComponentName(for kind: ComponentKind, in project: Project) -> String {
        let existing = Set(project.manifest.components.map(\.name))
        let base = switch kind {
        case .lua: "code"
        case .snippet: "snippet"
        case .tiles: "sprites"
        case .audio: "music"
        }
        var candidate = base
        var counter = 2
        while existing.contains(candidate) {
            candidate = "\(base)-\(counter)"
            counter += 1
        }
        return candidate
    }

    // MARK: Build and run

    @discardableResult
    func build() -> BuildProduct? {
        saveNow()
        guard let project else { return nil }
        do {
            let start = Date()
            let product = try ProjectBuilder.build(project)
            lastBuildURL = product.outputURL
            let ms = Int(Date().timeIntervalSince(start) * 1000)
            appendConsole("Built \(product.componentCount) component(s) -> \(product.outputURL.path) (\(ms) ms)\n")
            return product
        } catch {
            appendConsole("Build failed: \(error.localizedDescription)\n")
            errorMessage = "Build failed: \(error.localizedDescription)"
            return nil
        }
    }

    func run() {
        stop()
        guard let project, let product = build() else { return }
        refreshSimulator()
        guard let simulator = simulatorURL else {
            errorMessage = """
                Simulator not found. Build it with:
                cmake -S simulator -B simulator/build && cmake --build simulator/build
                (or set its path in Settings).
                """
            return
        }
        do {
            let session = try Runner.prepare(project: project, build: product)
            let task = Process()
            task.executableURL = simulator
            task.arguments = Runner.simulatorArguments(for: session)

            let pipe = Pipe()
            task.standardOutput = pipe
            task.standardError = pipe
            let continuation = consoleContinuation
            pipe.fileHandleForReading.readabilityHandler = { handle in
                let data = handle.availableData
                guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else {
                    return
                }
                continuation.yield(text)
            }
            task.terminationHandler = { [weak self] finished in
                let status = finished.terminationStatus
                Task { @MainActor in
                    self?.isRunning = false
                    self?.appendConsole("Simulator exited (status \(status))\n")
                }
            }
            try task.run()
            process = task
            isRunning = true
            appendConsole("Running \(project.programFileName) in \(simulator.path)\n")
        } catch {
            errorMessage = "Cannot launch the simulator: \(error.localizedDescription)"
        }
    }

    func stop() {
        if let process, process.isRunning {
            process.terminate()
            appendConsole("Simulator stopped\n")
        }
        process = nil
        isRunning = false
    }

    func revealBuildOutput() {
        guard let url = lastBuildURL else { return }
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    func clearConsole() {
        console = ""
    }

    // MARK: Simulator

    func refreshSimulator() {
        let starts = Runner.defaultSimulatorSearchStarts(
            executable: Bundle.main.executableURL,
            cwd: URL(fileURLWithPath: FileManager.default.currentDirectoryPath))
        simulatorURL = SimulatorLocator.locate(
            startingAt: starts,
            explicitPath: simulatorPathPreference.isEmpty ? nil : simulatorPathPreference)
    }

    private func appendConsole(_ text: String) {
        console += text
        // Keep the log bounded.
        if console.count > 60_000 {
            console = String(console.suffix(40_000))
        }
    }
}

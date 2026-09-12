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

    // Compile checking
    var compileDiagnostic: CompileDiagnostic?
    /// Non-nil when checks cannot run (shown in the editor banner).
    var compileCheckUnavailableReason: String?
    private var checkTask: Task<Void, Never>?
    private var lastReportedDiagnostic: CompileDiagnostic?

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
    private var fileWatchTask: Task<Void, Never>?
    private var observedLuaData: Data?

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

    /// The component whose content is currently loaded in the editors.
    /// Saves always target this component, never the newly selected one.
    private var editingComponent: ComponentRef?

    func componentSelectionChanged() {
        saveNow()
        loadSelectedComponent()
    }

    private func loadSelectedComponent() {
        fileWatchTask?.cancel()
        observedLuaData = nil
        guard let project, let component = selectedComponent else {
            editingComponent = nil
            return
        }
        editingComponent = component
        do {
            switch component.kind {
            case .lua, .snippet:
                let data = try Data(contentsOf: project.fileURL(for: component))
                luaText = String(data: data, encoding: .utf8) ?? ""
                observedLuaData = data
            case .tiles:
                tilesAsset = try ProjectStore.readTiles(component, in: project)
            case .audio:
                audioAsset = try ProjectStore.readAudio(component, in: project)
            }
            restartFileWatch()
        } catch {
            errorMessage = "Cannot read \(component.file): \(error.localizedDescription)"
        }
    }

    // MARK: Editing / saving

    /// Debounced save of the component currently loaded in the editors.
    func scheduleSave() {
        guard let component = editingComponent, let project else { return }
        saveTask?.cancel()
        saveTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled, let self,
                  self.editingComponent?.id == component.id
            else { return }
            self.save(component: component, in: project)
        }
        scheduleCheck()
    }

    func saveNow() {
        saveTask?.cancel()
        guard let component = editingComponent, let project else { return }
        save(component: component, in: project)
    }

    private func save(component: ComponentRef, in project: Project) {
        do {
            switch component.kind {
            case .lua, .snippet:
                try ProjectStore.writeText(luaText, to: project.fileURL(for: component))
                observedLuaData = Data(luaText.utf8)
            case .tiles:
                try ProjectStore.writeTiles(tilesAsset, for: component, in: project)
            case .audio:
                try ProjectStore.writeAudio(audioAsset, for: component, in: project)
            }
        } catch {
            errorMessage = "Cannot save \(component.file): \(error.localizedDescription)"
        }
    }

    private func restartFileWatch() {
        fileWatchTask?.cancel()
        guard let component = editingComponent,
              component.kind == .lua || component.kind == .snippet
        else { return }
        fileWatchTask = Task { [weak self] in
            while !Task.isCancelled {
                do {
                    try await Task.sleep(for: .milliseconds(500))
                } catch {
                    return
                }
                guard let self, !Task.isCancelled else { return }
                self.checkForExternalLuaChange()
            }
        }
    }

    private func checkForExternalLuaChange() {
        guard let project, let component = editingComponent,
              component.kind == .lua || component.kind == .snippet,
              let data = try? Data(contentsOf: project.fileURL(for: component)),
              data != observedLuaData
        else { return }

        let previousData = observedLuaData
        observedLuaData = data
        guard let text = String(data: data, encoding: .utf8) else { return }
        if Data(luaText.utf8) == previousData {
            luaText = text
            appendConsole("Reloaded \(component.file) after an external change\n")
            scheduleCheck()
        } else {
            errorMessage = "\(component.file) changed outside the IDE while it has unsaved edits"
            appendConsole("External change detected in \(component.file); keeping editor text\n")
        }
    }

    // MARK: Project management

    func createProject(named name: String, in parent: URL, interactive: Bool = true) {
        do {
            let created = try ProjectStore.createProject(
                named: name, in: parent, interactive: interactive)
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
        saveTask?.cancel()
        fileWatchTask?.cancel()
        checkTask?.cancel()
        editingComponent = nil // never save the previous project's buffer here
        self.project = project
        selectedComponentID = project.manifest.components.first?.id
        loadSelectedComponent()
        lastBuildURL = project.buildProductURL
        compileDiagnostic = nil
        scheduleCheck()
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
            // Drop the buffer first so the selection change does not save
            // the removed file back to disk.
            saveTask?.cancel()
            editingComponent = nil
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

    // MARK: Compile checking

    /// Debounced compile check of the freshly built project (saves first).
    func scheduleCheck() {
        checkTask?.cancel()
        checkTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(700))
            guard !Task.isCancelled, let self else { return }
            await self.performCompileCheck()
        }
    }

    /// Builds in memory, compiles with the OS's own Lua (via the
    /// simulator's --check mode) and maps any error back to a component.
    func performCompileCheck() async {
        saveNow()
        guard let project else {
            compileDiagnostic = nil
            return
        }
        let rendered: (lua: String, lineMap: LineMap)
        do {
            rendered = try ProjectBuilder.renderProduct(project)
        } catch {
            compileDiagnostic = nil
            return
        }
        refreshSimulator()
        guard let simulator = simulatorURL else {
            compileDiagnostic = nil
            let reason = "simulator not found — set its path in Settings"
            if compileCheckUnavailableReason != reason {
                compileCheckUnavailableReason = reason
                appendConsole("Compile checks unavailable: \(reason)\n")
            }
            return
        }
        compileCheckUnavailableReason = nil
        let source = rendered.lua
        let outcome = await Task.detached(priority: .utility) {
            LuaChecker.check(source: source, simulator: simulator)
        }.value

        if let reason = outcome.unavailableReason {
            compileDiagnostic = nil
            compileCheckUnavailableReason = reason
            appendConsole("Compile check unavailable: \(reason)\n")
            return
        }
        guard let error = outcome.error else {
            if compileDiagnostic != nil {
                appendConsole("Compile ok\n")
            }
            compileDiagnostic = nil
            lastReportedDiagnostic = nil
            return
        }

        // Attribute the error: Lua's own line first, then the "at line N"
        // context it adds for unclosed constructs, then a block-structure
        // hint (missing `end` usually points at the opener, not at <eof>).
        var line = error.line
        var location = line.flatMap { rendered.lineMap.source(forGeneratedLine: $0) }
        var message = error.message
        if location == nil, let context = error.contextLine {
            location = rendered.lineMap.source(forGeneratedLine: context)
            if location != nil {
                line = context
            }
        }
        if location == nil, let issue = LuaStructureChecker.check(rendered.lua) {
            location = rendered.lineMap.source(forGeneratedLine: issue.line)
            if location != nil {
                line = issue.line
                message = "\(issue.message) — Lua: \(error.message)"
            }
        }
        let diagnostic = CompileDiagnostic(
            message: message, generatedLine: error.line, location: location)
        compileDiagnostic = diagnostic
        if diagnostic != lastReportedDiagnostic {
            lastReportedDiagnostic = diagnostic
            appendConsole("Compile error: \(describe(diagnostic, in: project))\n")
        }
    }

    private func describe(_ diagnostic: CompileDiagnostic, in project: Project) -> String {
        var where_ = "generated code"
        if let location = diagnostic.location,
           let component = project.manifest.components.first(where: { $0.id == location.componentID }) {
            where_ = "\(component.name):\(location.line)"
        }
        var text = "\(where_): \(diagnostic.message)"
        if let generated = diagnostic.generatedLine {
            text += " (generated line \(generated))"
        }
        return text
    }

    @discardableResult
    func build() -> BuildProduct? {
        saveNow()
        guard let project else { return nil }
        do {
            let start = Date()
            let product = try ProjectBuilder.build(project)
            lastBuildURL = product.outputURL
            refreshSimulator()
            let fileManager = FileManager.default
            try? fileManager.removeItem(at: project.prgProductURL)
            var compiledMessage = ""
            if let simulator = simulatorURL {
                let outcome = PrgCompiler.compile(
                    source: product.outputURL,
                    output: project.prgProductURL,
                    simulator: simulator)
                if outcome.outputURL != nil {
                    compiledMessage = " + \(project.prgProductURL.path)"
                } else if let error = outcome.error {
                    appendConsole(".prg compilation failed: \(error)\n")
                } else if let reason = outcome.unavailableReason {
                    appendConsole(".prg compilation unavailable: \(reason)\n")
                }
            } else {
                appendConsole(".prg compilation unavailable: simulator not found\n")
            }
            let ms = Int(Date().timeIntervalSince(start) * 1000)
            appendConsole("Built \(product.componentCount) component(s) -> \(product.outputURL.path)\(compiledMessage) (\(ms) ms)\n")
            Task { await performCompileCheck() }
            return product
        } catch {
            appendConsole("Build failed: \(error.localizedDescription)\n")
            errorMessage = "Build failed: \(error.localizedDescription)"
            return nil
        }
    }

    func run() async {
        stop()
        guard let project, let product = build() else { return }
        await performCompileCheck()
        if let diagnostic = compileDiagnostic {
            let message = "Not running: \(describe(diagnostic, in: project))"
            appendConsole(message + "\n")
            errorMessage = message
            return
        }
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

    // MARK: Editor diagnostics

    /// Reports the live editor view hierarchy and its rendered pixels to
    /// the console, for debugging "text is invisible" on a real GUI.
    func dumpEditorDiagnostics() {
        guard let window = NSApp.keyWindow
            ?? NSApp.windows.first(where: { $0.isVisible }) else {
            appendConsole("editor dump: no window\n")
            return
        }
        guard let scrollView = Self.findEditorScrollView(in: window.contentView),
              let textView = scrollView.documentView as? NSTextView else {
            appendConsole("editor dump: no NSTextView in the window\n")
            return
        }
        var lines = ["editor dump:"]
        lines.append("  window: \(window.frame.size) key=\(window.isKeyWindow)")
        lines.append("  scroll: frame=\(scrollView.frame) hidden=\(scrollView.isHidden) alpha=\(scrollView.alphaValue)")
        lines.append("  clip:   bounds=\(scrollView.contentView.bounds)")
        lines.append("  frame:  clipFrame=\(scrollView.contentView.frame) rulerFrame=\(scrollView.verticalRulerView?.frame ?? .zero)")
        lines.append("  text:   frame=\(textView.frame) len=\((textView.string as NSString).length) hidden=\(textView.isHidden) alpha=\(textView.alphaValue)")
        lines.append("  text:   drawsBackground=\(textView.drawsBackground) textColor=\(textView.textColor?.description ?? "nil")")
        lines.append("  text:   backgroundColor=\(textView.backgroundColor.description) font=\(textView.font?.description ?? "nil")")
        lines.append("  appearance: \(textView.effectiveAppearance.name.rawValue)")
        lines.append("  layer: wants=\(textView.wantsLayer) has=\(textView.layer != nil)")

        if let layoutManager = textView.layoutManager,
           let container = textView.textContainer {
            layoutManager.ensureLayout(for: container)
            lines.append("  layout: glyphs=\(layoutManager.numberOfGlyphs) used=\(layoutManager.usedRect(for: container)) container=\(container.containerSize)")
        }
        if let storage = textView.textStorage, storage.length > 0 {
            let attrs = storage.attributes(at: 0, effectiveRange: nil)
            lines.append("  storage: attrs@0 font=\(attrs[.font] ?? "nil") color=\(attrs[.foregroundColor] ?? "nil")")
        }

        // 1. Normal render of the whole scroll view.
        if let rep = scrollView.bitmapImageRepForCachingDisplay(in: scrollView.bounds) {
            scrollView.cacheDisplay(in: scrollView.bounds, to: rep)
            let stats = Self.nonBackgroundStats(rep)
            lines.append("  pixels(scroll): \(stats.count) bbox=\(stats.bounds)")
            let path = "/tmp/spiide-editor-dump.png"
            if let png = rep.representation(using: .png, properties: [:]) {
                try? png.write(to: URL(fileURLWithPath: path))
                lines.append("  png: \(path)")
            }
        }

        // 2. Render the text view alone.
        if let rep = textView.bitmapImageRepForCachingDisplay(in: textView.bounds) {
            textView.cacheDisplay(in: textView.bounds, to: rep)
            let stats = Self.nonBackgroundStats(rep)
            lines.append("  pixels(textView): \(stats.count) bbox=\(stats.bounds)")
        }

        // 3. Force a concrete (non-dynamic) colour and re-render: if the
        //    text appears, dynamic colours are resolving invisibly here.
        if let storage = textView.textStorage, storage.length > 0 {
            let full = NSRange(location: 0, length: storage.length)
            storage.addAttribute(.foregroundColor, value: NSColor.red, range: full)
            if let rep = scrollView.bitmapImageRepForCachingDisplay(in: scrollView.bounds) {
                scrollView.cacheDisplay(in: scrollView.bounds, to: rep)
                let stats = Self.nonBackgroundStats(rep)
                lines.append("  pixels(red text): \(stats.count) bbox=\(stats.bounds)")
            }
            storage.removeAttribute(.foregroundColor, range: full)
            textView.needsDisplay = true
        }

        // 4. Screen capture of the window (ground truth, subject to the
        //    screen-recording permission).
        if let image = CGWindowListCreateImage(
            .null, .optionIncludingWindow, CGWindowID(window.windowNumber),
            [.boundsIgnoreFraming]) {
            let rep = NSBitmapImageRep(cgImage: image)
            let stats = Self.nonBackgroundStats(rep)
            lines.append("  pixels(screen): \(stats.count) bbox=\(stats.bounds)")
            if let png = rep.representation(using: .png, properties: [:]) {
                try? png.write(to: URL(fileURLWithPath: "/tmp/spiide-window.png"))
                lines.append("  screen png: /tmp/spiide-window.png")
            }
        } else {
            lines.append("  pixels(screen): unavailable")
        }

        appendConsole(lines.joined(separator: "\n") + "\n")
    }

    private static func findEditorScrollView(in view: NSView?) -> NSScrollView? {
        guard let view else { return nil }
        if let scrollView = view as? NSScrollView,
           scrollView.documentView is NSTextView {
            return scrollView
        }
        for subview in view.subviews {
            if let found = findEditorScrollView(in: subview) {
                return found
            }
        }
        return nil
    }

    private static func nonBackgroundStats(_ rep: NSBitmapImageRep) -> (count: Int, bounds: String) {
        guard let background = rep.colorAt(x: rep.pixelsWide - 4, y: 4) else {
            return (-1, "none")
        }
        var count = 0
        var minX = rep.pixelsWide, minY = rep.pixelsHigh, maxX = -1, maxY = -1
        for y in 0..<rep.pixelsHigh {
            for x in 0..<rep.pixelsWide {
                guard let color = rep.colorAt(x: x, y: y) else { continue }
                let distance = abs(color.redComponent - background.redComponent)
                    + abs(color.greenComponent - background.greenComponent)
                    + abs(color.blueComponent - background.blueComponent)
                if distance > 0.3 {
                    count += 1
                    minX = min(minX, x)
                    minY = min(minY, y)
                    maxX = max(maxX, x)
                    maxY = max(maxY, y)
                }
            }
        }
        guard count > 0 else { return (0, "none") }
        return (count, "(\(minX),\(minY))-(\(maxX),\(maxY)) of \(rep.pixelsWide)x\(rep.pixelsHigh)")
    }

    func clearConsole() {
        console = ""
    }

    // MARK: Simulator

    func refreshSimulator() {
        var starts = Runner.defaultSimulatorSearchStarts(
            executable: Bundle.main.executableURL,
            cwd: URL(fileURLWithPath: FileManager.default.currentDirectoryPath))
        // Works when built from Xcode (DerivedData lives outside the
        // workspace): the source path is baked in at compile time.
        starts.append(URL(fileURLWithPath: #filePath))
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

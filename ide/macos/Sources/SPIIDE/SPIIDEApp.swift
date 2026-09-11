import SwiftUI

import SPIIDECore

@main
struct SPIIDEApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @State private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(model)
        }
        .defaultSize(width: 1150, height: 780)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("New Project…") { model.showingNewProject = true }
                    .keyboardShortcut("n")
                Button("Open Project…") { model.showingOpenPanel = true }
                    .keyboardShortcut("o")
            }
            CommandGroup(after: .pasteboard) {
                Divider()
                Button("Complete") {
                    NSApp.sendAction(#selector(NSTextView.complete(_:)), to: nil, from: nil)
                }
                .keyboardShortcut(KeyEquivalent(" "), modifiers: .control)
            }
            CommandMenu("Project") {
                Button("Build") { model.build() }
                    .keyboardShortcut("b")
                Button(model.isRunning ? "Stop" : "Run in Simulator") {
                    if model.isRunning {
                        model.stop()
                    } else {
                        Task { await model.run() }
                    }
                }
                .keyboardShortcut("r")
            }
            CommandMenu("Developer") {
                Button("Dump Editor Diagnostics") {
                    model.dumpEditorDiagnostics()
                }
            }
        }

        Settings {
            SettingsView()
                .environment(model)
        }
    }
}

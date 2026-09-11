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
            CommandMenu("Project") {
                Button("Build") { model.build() }
                    .keyboardShortcut("b")
                Button(model.isRunning ? "Stop" : "Run in Simulator") {
                    model.isRunning ? model.stop() : model.run()
                }
                .keyboardShortcut("r")
            }
        }

        Settings {
            SettingsView()
                .environment(model)
        }
    }
}

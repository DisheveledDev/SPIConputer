import SwiftUI

@main
struct SPITerminalApp: App {
    init() {
        // Launched via `swift run` there is no app bundle, so the process
        // starts as a background agent; promote it to a regular app so the
        // window gets a menu bar and normal key focus.
        NSApplication.shared.setActivationPolicy(.regular)
    }

    var body: some Scene {
        WindowGroup("SPI Terminal") {
            ContentView()
                .frame(minWidth: 640, minHeight: 540)
                .onAppear { NSApp.activate(ignoringOtherApps: true) }
        }
    }
}

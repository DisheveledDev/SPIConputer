import AppKit

/// Activates the app when launched from a terminal (`swift run`) or in a
/// non-UI context, so the window accepts keyboard input instead of
/// waiting for an extra click.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        activate()
        // The window may not exist on the first attempt; retry shortly.
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(300))
            self?.activate()
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    private func activate() {
        NSApp.activate(ignoringOtherApps: true)
        NSApp.windows.first?.makeKeyAndOrderFront(nil)
    }
}

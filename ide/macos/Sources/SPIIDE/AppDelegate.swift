import AppKit

/// Activates the app when launched from a terminal (`swift run`) or in a
/// non-UI context, so the window accepts keyboard input instead of
/// waiting for an extra click.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        applyAppIcon()
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

    /// `swift run` produces a bare executable with no bundle icon, so the
    /// Dock icon is set at launch from the packaged resource instead.
    private func applyAppIcon() {
        guard let url = Bundle.module.url(forResource: "AppIcon", withExtension: "png"),
              let icon = NSImage(contentsOf: url)
        else { return }
        NSApp.applicationIconImage = icon
    }
}

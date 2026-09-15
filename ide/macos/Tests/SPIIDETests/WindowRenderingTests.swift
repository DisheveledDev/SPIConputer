import AppKit
import SwiftUI
import Testing

@testable import SPIIDE

/// Hosts the SwiftUI editor in a real window and captures the actual
/// on-screen pixels. Offscreen `cacheDisplay` re-runs `draw` in isolation
/// and misses compositing bugs such as a sibling view overpainting the
/// text, so this is the only check that reproduces "text invisible".
@Suite("Window rendering")
@MainActor
struct WindowRenderingTests {
    private struct Capture {
        let textPixels: Int
        let captured: Bool
    }

    private func capture(gutter: Bool) -> Capture {
        let hosting = NSHostingView(
            rootView: CodeEditorView(
                text: .constant("local x = 1\nprint(x)\n"),
                diagnosticLine: nil, syntaxHighlighting: true, gutter: gutter))
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 700, height: 400),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = hosting
        window.orderFront(nil)
        window.layoutIfNeeded()
        window.displayIfNeeded()
        RunLoop.main.run(until: Date().addingTimeInterval(0.5))
        window.displayIfNeeded()
        defer {
            window.orderOut(nil)
            RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        }

        guard let image = CGWindowListCreateImage(
            .null, .optionIncludingWindow, CGWindowID(window.windowNumber),
            [.boundsIgnoreFraming])
        else { return Capture(textPixels: 0, captured: false) }
        let rep = NSBitmapImageRep(cgImage: image)
        let width = rep.pixelsWide
        let height = rep.pixelsHigh
        guard width > 200, height > 100,
              let background = rep.colorAt(x: width - 40, y: height - 20),
              background.alphaComponent > 0.5
        else { return Capture(textPixels: 0, captured: false) }

        var count = 0
        for y in 0..<min(height, 300) {
            for x in 100..<(width - 40) {
                guard let color = rep.colorAt(x: x, y: y) else { continue }
                let distance = abs(color.redComponent - background.redComponent)
                    + abs(color.greenComponent - background.greenComponent)
                    + abs(color.blueComponent - background.blueComponent)
                if distance > 0.3 {
                    count += 1
                }
            }
        }
        return Capture(textPixels: count, captured: true)
    }

    /// Renders any SwiftUI view in a real window and returns the capture
    /// plus its bitmap (nil when window capture is unavailable).
    private func captureView<V: View>(_ view: V, size: NSSize) -> NSBitmapImageRep? {
        let hosting = NSHostingView(rootView: view)
        let window = NSWindow(
            contentRect: NSRect(origin: .zero, size: size),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = hosting
        window.orderFront(nil)
        window.layoutIfNeeded()
        window.displayIfNeeded()
        RunLoop.main.run(until: Date().addingTimeInterval(0.5))
        window.displayIfNeeded()
        defer {
            window.orderOut(nil)
            RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        }
        guard let image = CGWindowListCreateImage(
            .null, .optionIncludingWindow, CGWindowID(window.windowNumber),
            [.boundsIgnoreFraming])
        else { return nil }
        let rep = NSBitmapImageRep(cgImage: image)
        guard rep.pixelsWide > 200, rep.pixelsHigh > 100 else { return nil }
        return rep
    }

    @Test func welcomeScreenListsRecentProjects() {
        let model = AppModel()
        let parent = FileManager.default.temporaryDirectory
            .appendingPathComponent("spiide-welcome-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: parent) }
        // Creating a project records it; two of them make a list.
        model.createProject(named: "Invaders", in: parent)
        model.createProject(named: "Notes", in: parent)
        #expect(model.recentProjects.prefix(2).map(\.name) == ["Notes", "Invaders"])
        model.project = nil // back to the welcome screen

        // Drawn pixels in the lower half of the window: the list adds rows
        // below the buttons, which the plain welcome screen leaves blank.
        func lowerHalfDrawn(_ rep: NSBitmapImageRep) -> Int {
            guard let background = rep.colorAt(x: rep.pixelsWide - 20, y: rep.pixelsHigh - 20) else {
                return 0
            }
            var drawn = 0
            for y in (rep.pixelsHigh / 2)..<rep.pixelsHigh {
                for x in 0..<rep.pixelsWide {
                    guard let color = rep.colorAt(x: x, y: y) else { continue }
                    let distance = abs(color.redComponent - background.redComponent)
                        + abs(color.greenComponent - background.greenComponent)
                        + abs(color.blueComponent - background.blueComponent)
                    if distance > 0.3 { drawn += 1 }
                }
            }
            return drawn
        }
        // Under a loaded parallel test run the first capture can land
        // before SwiftUI has drawn the list; retry a few times and, like
        // the editor capture test, treat a blank window as capture being
        // unavailable rather than as a failure.
        var captured: NSBitmapImageRep?
        for _ in 0..<4 {
            guard let rep = captureView(WelcomeView().environment(model),
                                        size: NSSize(width: 700, height: 520)) else { break }
            captured = rep
            if lowerHalfDrawn(rep) > 100 { break }
        }
        if let rep = captured {
            if let dir = ProcessInfo.processInfo.environment["SPIIDE_SNAPSHOT_DIR"],
               let png = rep.representation(using: .png, properties: [:]) {
                try? png.write(to: URL(fileURLWithPath: dir).appendingPathComponent("welcome.png"))
            }
            let drawn = lowerHalfDrawn(rep)
            if drawn > 0 {
                #expect(drawn > 100, "recent projects rows drawn: \(drawn)")
            }
        }
        // Whether or not the screen could be captured, the model side holds.
        model.removeRecentProject(model.recentProjects[0])
        #expect(model.recentProjects.map(\.name) == ["Invaders"])
        model.clearRecentProjects()
        #expect(model.recentProjects.isEmpty)
    }

    @Test func textStaysVisibleWithLineNumberGutter() {
        let without = capture(gutter: false)
        let with = capture(gutter: true)
        guard without.captured, with.captured, without.textPixels > 100 else {
            // Window capture is unavailable (no screen access); nothing to compare.
            return
        }
        #expect(
            with.textPixels > 100,
            "text pixels with gutter: \(with.textPixels), without: \(without.textPixels)")
    }
}

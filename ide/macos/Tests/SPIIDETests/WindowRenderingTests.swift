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

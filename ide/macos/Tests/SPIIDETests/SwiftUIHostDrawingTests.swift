import AppKit
import SwiftUI
import Testing

@testable import SPIIDE

/// Hosts the real SwiftUI view stack (NSHostingView) and renders it with
/// `cacheDisplay`, which does capture AppKit-hosted content. Catches
/// representable/layout regressions that leave the editor blank.
@Suite("SwiftUI host rendering")
@MainActor
struct SwiftUIHostDrawingTests {
    private func hostCache<V: View>(_ view: V, size: NSSize) -> NSBitmapImageRep? {
        let hosting = NSHostingView(rootView: view)
        hosting.frame = NSRect(origin: .zero, size: size)
        hosting.layoutSubtreeIfNeeded()
        guard let rep = hosting.bitmapImageRepForCachingDisplay(in: hosting.bounds) else {
            return nil
        }
        hosting.cacheDisplay(in: hosting.bounds, to: rep)
        return rep
    }

    private func nonBackgroundPixels(_ rep: NSBitmapImageRep, startX: Int) -> Int {
        guard let background = rep.colorAt(x: rep.pixelsWide - 4, y: 4) else {
            return -1
        }
        var count = 0
        for y in 0..<min(160, rep.pixelsHigh) {
            for x in startX..<rep.pixelsWide {
                guard let color = rep.colorAt(x: x, y: y) else { continue }
                let distance = abs(color.redComponent - background.redComponent)
                    + abs(color.greenComponent - background.greenComponent)
                    + abs(color.blueComponent - background.blueComponent)
                if distance > 0.3 {
                    count += 1
                }
            }
        }
        return count
    }

    @Test func hostedLuaEditorDrawsText() {
        let model = AppModel()
        let parent = FileManager.default.temporaryDirectory
            .appendingPathComponent("spiide-host-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: parent) }
        model.createProject(named: "HostDraw", in: parent)

        let view = LuaEditorView(diagnosticLine: nil)
            .environment(model)
            .frame(width: 700, height: 400)
        guard let rep = hostCache(view, size: NSSize(width: 700, height: 400)) else {
            Issue.record("could not render hosted view")
            return
        }
        let pixels = nonBackgroundPixels(rep, startX: 60)
        #expect(pixels > 100, "hosted Lua editor non-background pixels: \(pixels)")
    }
}

import AppKit
import Testing

@testable import SPIIDE

/// Renders the editor offscreen and counts non-background pixels, so
/// "the text is invisible" regressions are caught without a GUI.
@Suite("Editor drawing")
@MainActor
struct CodeEditorDrawingTests {
    private func nonBackgroundPixels(
        _ editor: CodeEditorFactory.Editor,
        width: CGFloat = 600,
        height: CGFloat = 300
    ) -> Int {
        let view = editor.scrollView
        view.frame = NSRect(x: 0, y: 0, width: width, height: height)
        view.layoutSubtreeIfNeeded()
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else {
            return -1
        }
        view.cacheDisplay(in: view.bounds, to: rep)
        // Background sample far from gutter and text.
        guard let background = rep.colorAt(x: rep.pixelsWide - 4, y: rep.pixelsHigh - 4) else {
            return -1
        }
        var count = 0
        // Skip the ruler/gutter region: only look at the text area.
        let startX = 50
        for y in 0..<min(120, rep.pixelsHigh) {
            for x in startX..<min(Int(width), rep.pixelsWide) {
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

    @Test func plainEditorDrawsText() {
        let editor = CodeEditorFactory.make(
            text: "local x = 1\nprint(x)\n", delegate: nil)
        let pixels = nonBackgroundPixels(editor)
        #expect(pixels > 100, "plain editor non-background pixels: \(pixels)")
    }

    @Test func highlightedEditorDrawsText() {
        let editor = CodeEditorFactory.make(
            text: "local x = 1\nprint(x)\n", delegate: nil)
        CodeEditorFactory.highlightSyntax(editor.textView)
        let pixels = nonBackgroundPixels(editor)
        #expect(pixels > 100, "highlighted editor non-background pixels: \(pixels)")
    }

    /// The previous implementation painted `labelColor` over the whole
    /// range as a temporary attribute before colouring tokens. This test
    /// pins down whether that made the text invisible.
    @Test func labelColorTemporaryAttributeDrawsText() {
        let editor = CodeEditorFactory.make(
            text: "local x = 1\nprint(x)\n", delegate: nil)
        let textView = editor.textView
        let full = NSRange(location: 0, length: (textView.string as NSString).length)
        textView.layoutManager?.addTemporaryAttribute(
            .foregroundColor, value: NSColor.labelColor, forCharacterRange: full)
        let pixels = nonBackgroundPixels(editor)
        #expect(pixels > 100, "labelColor temporary attribute pixels: \(pixels)")
    }
}

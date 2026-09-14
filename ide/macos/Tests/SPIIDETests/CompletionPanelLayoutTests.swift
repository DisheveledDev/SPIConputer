import AppKit
import Testing

import SPIIDECore

@testable import SPIIDE

@Suite("Completion panel layout")
@MainActor
struct CompletionPanelLayoutTests {
    private func show(_ items: [CompletionItem]) -> CompletionPanel {
        let panel = CompletionPanel()
        panel.show(
            items: items,
            near: NSRect(x: 100, y: 100, width: 2, height: 16))
        return panel
    }

    @Test func singleItemRowIsFullyVisible() {
        let panel = show([CompletionItem(name: "ScreenPaletteSet", detail: "(t)")])
        defer { panel.hide() }

        let table = panel.rowsView
        #expect(table.numberOfRows == 1)
        // The panel is tall enough that the only row is not clipped.
        #expect(table.visibleRect.contains(table.rect(ofRow: 0)))
        #expect(table.frame.height == table.rowHeight)
    }

    @Test func longListsShowSeveralRowsAndScrollTheRest() {
        let items = (0..<14).map { CompletionItem(name: "item\($0)", detail: "(x)") }
        let panel = show(items)
        defer { panel.hide() }

        let table = panel.rowsView
        #expect(table.numberOfRows == items.count)
        #expect(table.visibleRect.height >= table.rowHeight * 8)
        #expect(table.visibleRect.height < table.frame.height)
    }
}

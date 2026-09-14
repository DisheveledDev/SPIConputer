import AppKit

import SPIIDECore

/// Completion list shown under the caret while typing, with the expected
/// parameters beside each function name. The panel never becomes key and
/// ignores the mouse, so every key press, including Backspace, stays with
/// the editor; the coordinator drives the selection and acceptance from
/// `doCommandBy`.
@MainActor
final class CompletionPanel: NSObject {
    private let panel: NSPanel
    private let effect: NSVisualEffectView
    private let scrollView: NSScrollView
    private let tableView: NSTableView
    private var items: [CompletionItem] = []
    private var selectedIndex = 0

    private static let rowHeight: CGFloat = 20
    private static let maxVisibleRows = 10
    private static let maxWidth: CGFloat = 480
    private static let font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)

    var isVisible: Bool { panel.isVisible }

    /// The row list, for layout tests that verify the panel fits its rows.
    var rowsView: NSTableView { tableView }

    var selectedMatch: String? {
        items.indices.contains(selectedIndex) ? items[selectedIndex].name : nil
    }

    /// Parameters of the selected entry, when it declares any.
    var selectedDetail: String? {
        items.indices.contains(selectedIndex) ? items[selectedIndex].detail : nil
    }

    override init() {
        tableView = NSTableView()
        tableView.headerView = nil
        // `.plain` avoids the styled-table inset that leaves the first row
        // half-hidden when the panel only shows one or two rows.
        tableView.style = .plain
        tableView.rowHeight = Self.rowHeight
        tableView.intercellSpacing = NSSize(width: 0, height: 0)
        tableView.selectionHighlightStyle = .regular
        tableView.allowsEmptySelection = false
        tableView.addTableColumn(NSTableColumn(identifier: NSUserInterfaceItemIdentifier("name")))

        scrollView = NSScrollView()
        scrollView.drawsBackground = false
        scrollView.hasVerticalScroller = true
        scrollView.autohidesScrollers = true
        // Overlay scrollers float over the rows instead of taking width
        // (and, with a legacy scroller, clipping the text) from them.
        scrollView.scrollerStyle = .overlay
        scrollView.documentView = tableView
        scrollView.translatesAutoresizingMaskIntoConstraints = false

        effect = NSVisualEffectView()
        effect.material = .popover
        effect.blendingMode = .behindWindow
        effect.state = .active
        effect.wantsLayer = true
        effect.layer?.cornerRadius = 6
        effect.layer?.masksToBounds = true
        effect.addSubview(scrollView)

        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 180, height: 48),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered, defer: true)
        panel.isFloatingPanel = true
        panel.level = .floating
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.hidesOnDeactivate = true
        panel.isMovable = false
        panel.becomesKeyOnlyIfNeeded = true
        panel.ignoresMouseEvents = true
        panel.contentView = effect

        super.init()

        tableView.dataSource = self
        tableView.delegate = self
        NSLayoutConstraint.activate([
            scrollView.leadingAnchor.constraint(equalTo: effect.leadingAnchor, constant: 4),
            scrollView.trailingAnchor.constraint(equalTo: effect.trailingAnchor, constant: -4),
            scrollView.topAnchor.constraint(equalTo: effect.topAnchor, constant: 4),
            scrollView.bottomAnchor.constraint(equalTo: effect.bottomAnchor, constant: -4),
        ])
    }

    func show(items: [CompletionItem], near caretRect: NSRect) {
        update(items: items, near: caretRect)
        panel.orderFront(nil)
    }

    func update(items: [CompletionItem], near caretRect: NSRect?) {
        self.items = items
        selectedIndex = 0
        tableView.reloadData()
        tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
        resize()
        if let caretRect {
            position(near: caretRect)
        }
    }

    func move(near caretRect: NSRect) {
        guard isVisible else { return }
        position(near: caretRect)
    }

    func moveSelection(by delta: Int) {
        guard !items.isEmpty else { return }
        selectedIndex = min(max(selectedIndex + delta, 0), items.count - 1)
        tableView.selectRowIndexes(
            IndexSet(integer: selectedIndex), byExtendingSelection: false)
        tableView.scrollRowToVisible(selectedIndex)
    }

    func hide() {
        panel.orderOut(nil)
    }

    private func resize() {
        let rows = min(items.count, Self.maxVisibleRows)
        let widest = items
            .map { Self.attributedText(for: $0).size().width }
            .max() ?? 120
        panel.setContentSize(NSSize(
            width: min(max(widest + 28, 140), Self.maxWidth),
            height: CGFloat(rows) * Self.rowHeight + 8))
    }

    private func position(near caretRect: NSRect) {
        let size = panel.frame.size
        var origin = NSPoint(x: caretRect.minX, y: caretRect.minY - size.height - 2)
        if let screen = NSScreen.screens.first(where: { $0.frame.contains(caretRect.origin) })
            ?? NSScreen.main
        {
            let visible = screen.visibleFrame
            origin.x = min(max(origin.x, visible.minX), max(visible.maxX - size.width, visible.minX))
            if origin.y < visible.minY {
                origin.y = caretRect.maxY + 2 // no room below: flip above
            }
        }
        panel.setFrameOrigin(origin)
    }

    private static func attributedText(for item: CompletionItem) -> NSAttributedString {
        let result = NSMutableAttributedString(
            string: item.name,
            attributes: [.font: font, .foregroundColor: NSColor.labelColor])
        if let detail = item.detail {
            result.append(NSAttributedString(
                string: "  " + detail,
                attributes: [.font: font, .foregroundColor: NSColor.secondaryLabelColor]))
        }
        return result
    }
}

extension CompletionPanel: NSTableViewDataSource, NSTableViewDelegate {
    func numberOfRows(in tableView: NSTableView) -> Int {
        items.count
    }

    func tableView(
        _ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int
    ) -> NSView? {
        let identifier = NSUserInterfaceItemIdentifier("completion")
        let cell = tableView.makeView(withIdentifier: identifier, owner: nil)
            as? NSTableCellView ?? {
                let cell = NSTableCellView()
                cell.identifier = identifier
                let field = NSTextField(labelWithString: "")
                field.font = Self.font
                field.lineBreakMode = .byTruncatingTail
                field.translatesAutoresizingMaskIntoConstraints = false
                cell.addSubview(field)
                cell.textField = field
                NSLayoutConstraint.activate([
                    field.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 4),
                    field.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -4),
                    field.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
                ])
                return cell
            }()
        cell.textField?.attributedStringValue = Self.attributedText(for: items[row])
        return cell
    }
}

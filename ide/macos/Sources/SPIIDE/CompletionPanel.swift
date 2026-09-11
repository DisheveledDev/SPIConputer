import AppKit

/// Completion list shown under the caret while typing. The panel never
/// becomes key and ignores the mouse, so every key press, including
/// Backspace, stays with the editor; the coordinator drives the selection
/// and acceptance from `doCommandBy`.
@MainActor
final class CompletionPanel: NSObject {
    private let panel: NSPanel
    private let effect: NSVisualEffectView
    private let scrollView: NSScrollView
    private let tableView: NSTableView
    private var matches: [String] = []
    private var selectedIndex = 0

    private static let rowHeight: CGFloat = 18
    private static let maxVisibleRows = 8
    private static let font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)

    var isVisible: Bool { panel.isVisible }

    var selectedMatch: String? {
        matches.indices.contains(selectedIndex) ? matches[selectedIndex] : nil
    }

    override init() {
        tableView = NSTableView()
        tableView.headerView = nil
        tableView.rowHeight = Self.rowHeight
        tableView.intercellSpacing = NSSize(width: 0, height: 0)
        tableView.selectionHighlightStyle = .regular
        tableView.allowsEmptySelection = false
        tableView.addTableColumn(NSTableColumn(identifier: NSUserInterfaceItemIdentifier("name")))

        scrollView = NSScrollView()
        scrollView.drawsBackground = false
        scrollView.hasVerticalScroller = true
        scrollView.autohidesScrollers = true
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
            contentRect: NSRect(x: 0, y: 0, width: 160, height: 40),
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
            scrollView.leadingAnchor.constraint(equalTo: effect.leadingAnchor, constant: 3),
            scrollView.trailingAnchor.constraint(equalTo: effect.trailingAnchor, constant: -3),
            scrollView.topAnchor.constraint(equalTo: effect.topAnchor, constant: 3),
            scrollView.bottomAnchor.constraint(equalTo: effect.bottomAnchor, constant: -3),
        ])
    }

    func show(matches: [String], near caretRect: NSRect) {
        update(matches: matches, near: caretRect)
        panel.orderFront(nil)
    }

    func update(matches: [String], near caretRect: NSRect?) {
        self.matches = matches
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
        guard !matches.isEmpty else { return }
        selectedIndex = min(max(selectedIndex + delta, 0), matches.count - 1)
        tableView.selectRowIndexes(
            IndexSet(integer: selectedIndex), byExtendingSelection: false)
        tableView.scrollRowToVisible(selectedIndex)
    }

    func hide() {
        panel.orderOut(nil)
    }

    private func resize() {
        let rows = min(matches.count, Self.maxVisibleRows)
        let widest = matches
            .map { ($0 as NSString).size(withAttributes: [.font: Self.font]).width }
            .max() ?? 80
        panel.setContentSize(NSSize(
            width: min(max(widest + 24, 100), 320),
            height: CGFloat(rows) * Self.rowHeight + 6))
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
}

extension CompletionPanel: NSTableViewDataSource, NSTableViewDelegate {
    func numberOfRows(in tableView: NSTableView) -> Int {
        matches.count
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
        cell.textField?.stringValue = matches[row]
        return cell
    }
}

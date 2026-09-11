import AppKit

import SPIIDECore

/// Floating parameter-info strip shown below the caret while the caret is
/// inside a function call's argument list (signature help). It never
/// takes key focus, so typing and deletion stay with the editor.
@MainActor
final class SignatureHelpPanel {
    private let panel: NSPanel
    private let label: NSTextField
    private let effect: NSVisualEffectView

    var isVisible: Bool { panel.isVisible }
    var displayText: String { label.stringValue }

    init() {
        label = NSTextField(labelWithString: "")
        label.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        label.lineBreakMode = .byClipping
        label.translatesAutoresizingMaskIntoConstraints = false

        effect = NSVisualEffectView()
        effect.material = .popover
        effect.blendingMode = .behindWindow
        effect.state = .active
        effect.wantsLayer = true
        effect.layer?.cornerRadius = 6
        effect.layer?.masksToBounds = true
        effect.addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: effect.leadingAnchor, constant: 8),
            label.trailingAnchor.constraint(equalTo: effect.trailingAnchor, constant: -8),
            label.topAnchor.constraint(equalTo: effect.topAnchor, constant: 4),
            label.bottomAnchor.constraint(equalTo: effect.bottomAnchor, constant: -4),
        ])

        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 10, height: 10),
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
    }

    /// Shows the signature with the active parameter emphasised, placed
    /// under `caretRect` (screen coordinates).
    func show(context: LuaSignatureHelp.Context, near caretRect: NSRect) {
        label.attributedStringValue = Self.attributedText(
            signature: context.signature, active: context.activeParameter)
        let size = NSSize(
            width: ceil(label.intrinsicContentSize.width) + 16,
            height: ceil(label.intrinsicContentSize.height) + 8)
        panel.setContentSize(size)

        var origin = NSPoint(x: caretRect.minX, y: caretRect.minY - size.height - 4)
        if let screen = NSScreen.screens.first(where: { $0.frame.contains(caretRect.origin) })
            ?? NSScreen.main
        {
            let visible = screen.visibleFrame
            origin.x = min(max(origin.x, visible.minX), max(visible.maxX - size.width, visible.minX))
            if origin.y < visible.minY {
                origin.y = caretRect.maxY + 4 // no room below: flip above
            }
        }
        panel.setFrameOrigin(origin)
        panel.orderFront(nil)
    }

    func hide() {
        panel.orderOut(nil)
    }

    static func attributedText(
        signature: LuaSignature, active: Int?
    ) -> NSAttributedString {
        let regular = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        let emphasised = NSFont.monospacedSystemFont(ofSize: 11, weight: .semibold)
        let plain: [NSAttributedString.Key: Any] = [
            .font: regular, .foregroundColor: NSColor.secondaryLabelColor,
        ]
        let result = NSMutableAttributedString()
        result.append(NSAttributedString(string: "\(signature.name)(", attributes: plain))
        for (index, parameter) in signature.parameters.enumerated() {
            if index > 0 {
                result.append(NSAttributedString(string: ", ", attributes: plain))
            }
            let isActive = index == active
            result.append(NSAttributedString(
                string: parameter,
                attributes: [
                    .font: isActive ? emphasised : regular,
                    .foregroundColor: isActive ? NSColor.labelColor : NSColor.secondaryLabelColor,
                ]))
        }
        result.append(NSAttributedString(string: ")", attributes: plain))
        return result
    }
}

import AppKit

/// Container for the editor: an optional line-number gutter beside the
/// scrolling text view. A plain container avoids the NSRulerView problems
/// in SwiftUI hosting.
final class EditorContainerView: NSView {
    static let gutterWidth: CGFloat = 44

    let scrollView: NSScrollView
    let gutter: LineNumberGutterView?
    let textView: NSTextView

    init(scrollView: NSScrollView, textView: NSTextView, gutter: LineNumberGutterView?) {
        self.scrollView = scrollView
        self.textView = textView
        self.gutter = gutter
        super.init(frame: scrollView.frame)
        clipsToBounds = true
        addSubview(scrollView)
        if let gutter {
            addSubview(gutter)
        }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override var isFlipped: Bool {
        true
    }

    override func layout() {
        super.layout()
        let width = Self.gutterWidth
        let hasGutter = gutter != nil
        gutter?.frame = NSRect(x: 0, y: 0, width: width, height: bounds.height)
        scrollView.frame = NSRect(
            x: hasGutter ? width : 0, y: 0,
            width: max(bounds.width - (hasGutter ? width : 0), 0),
            height: bounds.height)
        CodeEditorFactory.refit(textView, in: scrollView)
        gutter?.needsDisplay = true
    }
}

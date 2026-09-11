import AppKit

/// Line-number gutter drawn as a plain view next to the scroll view.
/// Deliberately not an NSRulerView: the ruler machinery broke the scroll
/// view's tiling in this hosting setup (text stopped drawing).
final class LineNumberGutterView: NSView {
    weak var textView: NSTextView?

    var errorLine: Int? {
        didSet { needsDisplay = true }
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        clipsToBounds = true
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override var isFlipped: Bool {
        true
    }

    /// Since macOS 14 views no longer clip to their bounds by default, so
    /// `dirtyRect` can extend over the sibling text view; only paint within
    /// our own bounds.
    override func draw(_ dirtyRect: NSRect) {
        let paintRect = bounds.intersection(dirtyRect)
        NSColor.textBackgroundColor.setFill()
        paintRect.fill()
        NSColor.separatorColor.setFill()
        NSRect(x: bounds.maxX - 1, y: paintRect.minY, width: 1, height: paintRect.height).fill()

        guard let textView,
              let layoutManager = textView.layoutManager,
              let textContainer = textView.textContainer
        else { return }
        let text = textView.string as NSString
        let normal: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: 10, weight: .regular),
            .foregroundColor: NSColor.secondaryLabelColor,
        ]
        let error: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: 10, weight: .bold),
            .foregroundColor: NSColor.systemRed,
        ]

        layoutManager.ensureLayout(for: textContainer)
        let visibleRect = textView.visibleRect
        let glyphRange = layoutManager.glyphRange(
            forBoundingRect: visibleRect, in: textContainer)
        if glyphRange.length == 0 {
            draw(number: "1", atY: textView.textContainerOrigin.y, attributes: normal)
            return
        }

        let firstGlyph = min(glyphRange.location, layoutManager.numberOfGlyphs - 1)
        var characterIndex = layoutManager.characterIndexForGlyph(at: firstGlyph)
        var lineNumber = 1
        if characterIndex > 0 {
            let prefix = text.substring(to: min(characterIndex, text.length))
            lineNumber += prefix.reduce(0) { $0 + ($1 == "\n" ? 1 : 0) }
        }
        var lastLineStart = -1
        var glyphIndex = firstGlyph
        let lastGlyph = min(
            glyphRange.location + glyphRange.length,
            layoutManager.numberOfGlyphs)
        while glyphIndex < lastGlyph {
            var effectiveRange = NSRange()
            let fragmentRect = layoutManager.lineFragmentRect(
                forGlyphAt: glyphIndex, effectiveRange: &effectiveRange)
            characterIndex = layoutManager.characterIndexForGlyph(at: glyphIndex)
            var lineStart = 0
            var lineEnd = 0
            var contentsEnd = 0
            text.getLineStart(
                &lineStart, end: &lineEnd, contentsEnd: &contentsEnd,
                for: NSRange(location: min(characterIndex, text.length), length: 0))
            if lineStart != lastLineStart {
                let pointInTextView = NSPoint(
                    x: 0,
                    y: textView.textContainerOrigin.y + fragmentRect.minY)
                let y = convert(pointInTextView, from: textView).y
                if y + fragmentRect.height >= dirtyRect.minY,
                   y <= dirtyRect.maxY {
                    draw(
                        number: "\(lineNumber)",
                        atY: y + (fragmentRect.height - 12) / 2,
                        attributes: errorLine == lineNumber ? error : normal)
                }
                lastLineStart = lineStart
                lineNumber += 1
            }
            glyphIndex = max(glyphIndex + 1, effectiveRange.location + effectiveRange.length)
        }
    }

    private func draw(number: String, atY y: CGFloat, attributes: [NSAttributedString.Key: Any]) {
        let size = number.size(withAttributes: attributes)
        number.draw(
            at: NSPoint(x: bounds.maxX - size.width - 6, y: y), withAttributes: attributes)
    }
}

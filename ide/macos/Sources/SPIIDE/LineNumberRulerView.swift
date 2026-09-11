import AppKit

/// Gutter showing 1-based line numbers next to an NSTextView. The line
/// carrying a compile error is drawn in red.
final class LineNumberRulerView: NSRulerView {
    var errorLine: Int? {
        didSet { needsDisplay = true }
    }

    private weak var textView: NSTextView?

    init(textView: NSTextView) {
        self.textView = textView
        super.init(scrollView: textView.enclosingScrollView, orientation: .verticalRuler)
        clientView = textView
        ruleThickness = 42
    }

    @available(*, unavailable)
    required init(coder: NSCoder) {
        fatalError("init(coder:) is not supported")
    }

    override func drawHashMarksAndLabels(in rect: NSRect) {
        guard let textView,
              let layoutManager = textView.layoutManager,
              let textContainer = textView.textContainer,
              let scrollView
        else { return }

        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: rect).addClip()
        defer { NSGraphicsContext.restoreGraphicsState() }

        NSColor.textBackgroundColor.setFill()
        rect.fill()
        NSColor.separatorColor.setFill()
        NSRect(x: bounds.maxX - 1, y: rect.minY, width: 1, height: rect.height).fill()

        let text = textView.string as NSString
        let textLength = text.length
        let visibleRect = scrollView.contentView.bounds

        let normal: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: 10, weight: .regular),
            .foregroundColor: NSColor.secondaryLabelColor,
        ]
        let error: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: 10, weight: .bold),
            .foregroundColor: NSColor.systemRed,
        ]

        guard textLength > 0 else {
            draw("1", atY: textView.textContainerInset.height, attributes: normal)
            return
        }

        let glyphRange = layoutManager.glyphRange(forBoundingRect: visibleRect, in: textContainer)
        let charRange = layoutManager.characterRange(forGlyphRange: glyphRange, actualGlyphRange: nil)

        // 1-based number of the first visible line.
        var lineNumber = 1
        if charRange.location > 0 {
            let prefix = text.substring(to: min(charRange.location, textLength))
            lineNumber += prefix.reduce(0) { $0 + ($1 == "\n" ? 1 : 0) }
        }

        let relativePoint = convert(NSPoint.zero, from: textView)
        var index = min(charRange.location, textLength)

        for _ in 0..<2000 { // generous safety bound
            guard index < textLength else { break }
            var lineStart = 0
            var lineEnd = 0
            var contentsEnd = 0
            text.getLineStart(
                &lineStart, end: &lineEnd, contentsEnd: &contentsEnd,
                for: NSRange(location: index, length: 0))

            let glyphIndex = layoutManager.glyphIndexForCharacter(at: lineStart)
            let fragmentRect = layoutManager.lineFragmentRect(
                forGlyphAt: glyphIndex, effectiveRange: nil)
            let y = fragmentRect.minY + textView.textContainerInset.height + relativePoint.y

            if y > visibleRect.maxY + fragmentRect.height {
                break
            }
            if y + fragmentRect.height >= visibleRect.minY {
                let isError = errorLine == lineNumber
                draw("\(lineNumber)", atY: y + (fragmentRect.height - 12) / 2,
                     attributes: isError ? error : normal)
            }

            guard lineEnd > index else { break }
            index = lineEnd
            lineNumber += 1
        }
    }

    private func draw(_ number: String, atY y: CGFloat, attributes: [NSAttributedString.Key: Any]) {
        let size = number.size(withAttributes: attributes)
        let point = NSPoint(x: ruleThickness - size.width - 6, y: y)
        number.draw(at: point, withAttributes: attributes)
    }
}

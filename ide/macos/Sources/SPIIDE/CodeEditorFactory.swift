import AppKit

import SPIIDECore

/// Builds the code editor's AppKit views. Uses AppKit's own
/// `NSTextView.scrollableTextView()` setup (known-good sizing), with the
/// line-number ruler, syntax highlighting and error highlighting layered on
/// top. Extracted from the SwiftUI representable so it can be regression
/// tested headlessly.
@MainActor
enum CodeEditorFactory {
    struct Editor {
        let scrollView: NSScrollView
        let textView: NSTextView
        let ruler: LineNumberRulerView
    }

    static let indentUnit = "    "

    static func make(
        text: String,
        delegate: NSTextViewDelegate?,
        highlight: Bool = true
    ) -> Editor {
        let scrollView = NSTextView.scrollableTextView()
        guard let textView = scrollView.documentView as? NSTextView else {
            fatalError("scrollableTextView returned no text view")
        }
        textView.delegate = delegate
        textView.isRichText = false
        textView.allowsUndo = true
        textView.isAutomaticTextCompletionEnabled = false
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        textView.isAutomaticSpellingCorrectionEnabled = false
        textView.font = .monospacedSystemFont(ofSize: 13, weight: .regular)
        textView.textColor = .labelColor
        textView.drawsBackground = true
        textView.textContainerInset = NSSize(width: 4, height: 4)
        textView.string = text
        if highlight {
            highlightSyntax(textView)
        }

        let ruler = LineNumberRulerView(textView: textView)
        scrollView.verticalRulerView = ruler
        scrollView.hasVerticalRuler = true
        scrollView.rulersVisible = true

        return Editor(scrollView: scrollView, textView: textView, ruler: ruler)
    }

    // MARK: Highlighting

    /// Colours comments, strings, numbers, keywords and known functions.
    /// Uses real text-storage attributes (the reliable drawing path) and
    /// resets font/colour first so edits never leave stale colours.
    static func highlightSyntax(_ textView: NSTextView) {
        guard let storage = textView.textStorage else { return }
        let full = NSRange(location: 0, length: storage.length)
        let base: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 13, weight: .regular),
            .foregroundColor: NSColor.labelColor,
        ]
        storage.beginEditing()
        storage.setAttributes(base, range: full)
        if storage.length > 0 {
            for token in LuaTokenizer.tokenize(textView.string) {
                storage.addAttribute(
                    .foregroundColor, value: color(for: token.kind),
                    range: NSIntersectionRange(token.range, full))
            }
        }
        storage.endEditing()
        textView.typingAttributes = base
        textView.needsDisplay = true
    }

    /// Highlights the given 1-based line with a red background (or clears
    /// it when nil). Uses text-storage attributes so it draws reliably.
    static func applyDiagnostic(line: Int?, to textView: NSTextView) {
        guard let storage = textView.textStorage else { return }
        let full = NSRange(location: 0, length: storage.length)
        storage.beginEditing()
        storage.removeAttribute(.backgroundColor, range: full)
        if let line, let range = lineRange(line, in: textView.string) {
            storage.addAttribute(
                .backgroundColor, value: NSColor.systemRed.withAlphaComponent(0.18),
                range: NSIntersectionRange(range, full))
        }
        storage.endEditing()
    }

    static func lineRange(_ line: Int, in string: String) -> NSRange? {
        guard line >= 1 else { return nil }
        let text = string as NSString
        var location = 0
        var current = 1
        while current < line {
            let found = text.range(
                of: "\n", options: [],
                range: NSRange(location: location, length: text.length - location))
            guard found.location != NSNotFound else { return nil }
            location = found.location + found.length
            current += 1
        }
        guard location <= text.length else { return nil }
        let rest = text.range(
            of: "\n", options: [],
            range: NSRange(location: location, length: text.length - location))
        let end = rest.location == NSNotFound ? text.length : rest.location
        return NSRange(location: location, length: end - location)
    }

    static func color(for kind: LuaTokenizer.Kind) -> NSColor {
        switch kind {
        case .comment: .secondaryLabelColor
        case .string: .systemRed
        case .number: .systemPurple
        case .keyword: .systemPink
        case .function: .systemBlue
        }
    }

    // MARK: Auto-indent

    /// Indentation for a new line inserted at the caret: the current line's
    /// leading whitespace, plus one level after block openers.
    static func indentationForNewline(in textView: NSTextView) -> String {
        let ns = textView.string as NSString
        let caret = min(textView.selectedRange().location, ns.length)
        let lineRange = ns.lineRange(for: NSRange(location: caret, length: 0))
        let line = ns.substring(with: lineRange).trimmingCharacters(in: .newlines)
        let leading = String(line.prefix { $0 == " " || $0 == "\t" })
        let body = line.trimmingCharacters(in: .whitespaces)
        return opensBlock(body) ? leading + indentUnit : leading
    }

    /// After typing a closer (`end`, `until`, `else`, `elseif`, `}`, `)`),
    /// returns the indent range to remove when that closer is the start of
    /// its line. Pure so it can be unit tested.
    static func dedentRange(
        text: String,
        caret: Int,
        typed: Character,
        excludingLineStart excluded: Int
    ) -> (lineStart: Int, range: NSRange)? {
        guard "defl})".contains(typed) else { return nil }
        let ns = text as NSString
        guard caret > 0, caret <= ns.length else { return nil }
        let lineRange = ns.lineRange(for: NSRange(location: caret - 1, length: 0))
        guard lineRange.location != excluded else { return nil }
        let rawLine = ns.substring(with: lineRange)
        let line = rawLine.trimmingCharacters(in: .newlines)
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard closers.contains(trimmed) else { return nil }
        // Only when the caret sits at the end of the closer.
        let lineEndWithoutNewline = lineRange.location + (line as NSString).length
        guard caret >= lineEndWithoutNewline else { return nil }
        let leading = line.prefix { $0 == " " || $0 == "\t" }.count
        guard leading >= indentUnit.count else { return nil }
        return (lineRange.location,
                NSRange(location: lineRange.location, length: indentUnit.count))
    }

    private static let openers = ["then", "do", "function", "else", "repeat", "{", "("]
    private static let closers: Set<String> = ["end", "until", "else", "elseif", "}", ")"]

    private static func opensBlock(_ line: String) -> Bool {
        guard let last = line.last else { return false }
        if last == "{" || last == "(" {
            return true
        }
        if last == ")" && line.contains("function") {
            return true
        }
        return openers.contains { endsWithWord(line, $0) }
    }

    /// True when `line` ends with `word` as a whole word.
    private static func endsWithWord(_ line: String, _ word: String) -> Bool {
        guard line.hasSuffix(word) else { return false }
        guard let before = line.dropLast(word.count).last else { return true }
        return !(before.isLetter || before.isNumber || before == "_")
    }
}

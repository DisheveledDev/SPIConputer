import AppKit
import SwiftUI

import SPIIDECore

/// Monospaced code editor backed by NSTextView: line-number gutter,
/// compile-error line highlight, Lua/SPIComputer autocompletion and
/// structure-based auto-indent.
struct CodeEditorView: NSViewRepresentable {
    @Binding var text: String
    /// 1-based line in this component to highlight as an error.
    var diagnosticLine: Int?

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeNSView(context: Context) -> NSScrollView {
        let editor = CodeEditorFactory.make(text: text, delegate: context.coordinator)
        context.coordinator.textView = editor.textView
        context.coordinator.ruler = editor.ruler

        editor.scrollView.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(
            context.coordinator,
            selector: #selector(Coordinator.boundsChanged(_:)),
            name: NSView.boundsDidChangeNotification,
            object: editor.scrollView.contentView)

        return editor.scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        guard let textView = scrollView.documentView as? NSTextView else { return }
        context.coordinator.parent = self

        if textView.string != text {
            context.coordinator.isApplyingModel = true
            let selected = textView.selectedRange()
            textView.string = text
            let length = (text as NSString).length
            textView.setSelectedRange(
                NSRange(location: min(selected.location, length), length: 0))
            context.coordinator.isApplyingModel = false
            context.coordinator.highlightSyntax()
            context.coordinator.ruler?.needsDisplay = true
        }
        context.coordinator.applyDiagnostic(line: diagnosticLine)
    }

    @MainActor
    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: CodeEditorView
        weak var textView: NSTextView?
        weak var ruler: LineNumberRulerView?
        var isApplyingModel = false
        private var isHighlighting = false
        private var appliedDiagnosticLine: Int?
        private var lastDedentedLineStart = -1

        init(_ parent: CodeEditorView) {
            self.parent = parent
        }

        @objc func boundsChanged(_ notification: Notification) {
            ruler?.needsDisplay = true
        }

        // MARK: Text changes

        func textDidChange(_ notification: Notification) {
            guard !isHighlighting else { return } // our own attribute edits
            guard let textView = notification.object as? NSTextView else { return }
            if !isApplyingModel {
                parent.text = textView.string
            }
            dedentIfNeeded(in: textView)
            highlightSyntax()
            appliedDiagnosticLine = nil // highlighting resets backgrounds
            ruler?.needsDisplay = true
            scheduleCompletion(in: textView)
        }

        /// Return keeps the indentation, opening a level after block
        /// openers (function/if/for/while/do/else/repeat and `{`/`(`).
        func textView(_ textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
            guard commandSelector == #selector(NSResponder.insertNewline(_:)) else {
                return false
            }
            let indent = CodeEditorFactory.indentationForNewline(in: textView)
            textView.insertText("\n" + indent, replacementRange: textView.selectedRange())
            return true
        }

        private func dedentIfNeeded(in textView: NSTextView) {
            guard let storage = textView.textStorage,
                  storage.changeInLength == 1,
                  storage.editedRange.length == 1
            else { return }
            let typed = (textView.string as NSString).substring(with: storage.editedRange)
            guard typed.count == 1, let character = typed.first else { return }
            guard let dedent = CodeEditorFactory.dedentRange(
                text: textView.string,
                caret: storage.editedRange.location + storage.editedRange.length,
                typed: character,
                excludingLineStart: lastDedentedLineStart)
            else { return }
            lastDedentedLineStart = dedent.lineStart
            isHighlighting = true
            storage.replaceCharacters(in: dedent.range, with: "")
            isHighlighting = false
        }

        // MARK: Syntax highlighting

        func highlightSyntax() {
            guard let textView else { return }
            isHighlighting = true
            CodeEditorFactory.highlightSyntax(textView)
            isHighlighting = false
        }

        // MARK: Completion

        func textView(
            _ textView: NSTextView,
            completions words: [String],
            forPartialWordRange charRange: NSRange,
            indexOfSelectedItem index: UnsafeMutablePointer<Int>?
        ) -> [String] {
            guard let range = Range(charRange, in: textView.string) else { return words }
            let prefix = String(textView.string[range])
            let matches = LuaCompletion.matches(prefix)
            return matches.isEmpty ? words : matches
        }

        private var completionWork: DispatchWorkItem?

        private func scheduleCompletion(in textView: NSTextView) {
            completionWork?.cancel()
            guard let prefix = currentWord(in: textView), prefix.count >= 2 else { return }
            let matches = LuaCompletion.matches(prefix)
            guard !matches.isEmpty, matches.first != prefix else { return }
            let work = DispatchWorkItem { [weak self, weak textView] in
                guard let self, let textView,
                      textView.window?.isKeyWindow == true,
                      self.currentWord(in: textView) == prefix
                else { return }
                textView.complete(nil)
            }
            completionWork = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.15, execute: work)
        }

        private func currentWord(in textView: NSTextView) -> String? {
            let text = textView.string as NSString
            let location = textView.selectedRange().location
            guard location <= text.length else { return nil }
            let allowed = CharacterSet(
                charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.")
            var start = location
            while start > 0 {
                let character = text.character(at: start - 1)
                guard let scalar = UnicodeScalar(character), allowed.contains(scalar) else {
                    break
                }
                start -= 1
            }
            guard start < location else { return nil }
            return text.substring(with: NSRange(location: start, length: location - start))
        }

        // MARK: Diagnostics

        func applyDiagnostic(line: Int?) {
            guard let textView else { return }
            guard appliedDiagnosticLine != line else { return }
            appliedDiagnosticLine = line
            ruler?.errorLine = line
            isHighlighting = true
            CodeEditorFactory.applyDiagnostic(line: line, to: textView)
            isHighlighting = false
            if let line, let range = CodeEditorFactory.lineRange(line, in: textView.string) {
                textView.scrollRangeToVisible(range)
            }
        }
    }
}

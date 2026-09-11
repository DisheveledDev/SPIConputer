import AppKit
import SwiftUI

import SPIIDECore

extension Notification.Name {
    /// Posted by Edit ▸ Complete: the focused editor opens its
    /// completion list, or steps through it when it is already open.
    static let spiCompleteInEditor = Notification.Name("SPICompleteInEditor")
}

/// Monospaced code editor backed by NSTextView: line-number gutter,
/// compile-error line highlight, Lua/SPIComputer autocompletion and
/// structure-based auto-indent. The gutter is a plain sibling view (not an
/// NSRulerView, which broke scroll-view tiling in this hosting setup).
struct CodeEditorView: NSViewRepresentable {
    @Binding var text: String
    /// 1-based line in this component to highlight as an error.
    var diagnosticLine: Int?
    /// When off, no foreground attributes are written at all.
    var syntaxHighlighting = true
    /// When off, the line-number gutter is not shown at all.
    var gutter = true

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeNSView(context: Context) -> EditorContainerView {
        let editor = CodeEditorFactory.make(
            text: text, delegate: context.coordinator,
            highlight: syntaxHighlighting, showGutter: gutter)
        context.coordinator.textView = editor.textView
        context.coordinator.scrollView = editor.scrollView
        context.coordinator.gutter = editor.gutter
        context.coordinator.syncTextLength()

        editor.scrollView.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(
            context.coordinator,
            selector: #selector(Coordinator.boundsChanged(_:)),
            name: NSView.boundsDidChangeNotification,
            object: editor.scrollView.contentView)

        // The first layout happens after makeNSView returns; refit once the
        // window has settled (without this the document view can keep its
        // initial size and draw nothing).
        DispatchQueue.main.async { [weak coordinator = context.coordinator] in
            coordinator?.refitEditor()
        }

        return editor.container
    }

    func updateNSView(_ container: EditorContainerView, context: Context) {
        let textView = container.textView
        context.coordinator.parent = self

        if textView.string != text {
            context.coordinator.isApplyingModel = true
            let selected = textView.selectedRange()
            textView.string = text
            let length = (text as NSString).length
            textView.setSelectedRange(
                NSRange(location: min(selected.location, length), length: 0))
            context.coordinator.isApplyingModel = false
            context.coordinator.syncTextLength()
            context.coordinator.highlightSyntax()
            context.coordinator.gutter?.needsDisplay = true
        }
        context.coordinator.refitEditor()
        context.coordinator.applyDiagnostic(line: diagnosticLine)
        context.coordinator.updateSignatureHelp()
    }

    @MainActor
    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: CodeEditorView
        weak var textView: NSTextView?
        weak var scrollView: NSScrollView?
        weak var gutter: LineNumberGutterView?
        var isApplyingModel = false
        private var isHighlighting = false
        private var appliedDiagnosticLine: Int?
        private var lastDedentedLineStart = -1
        /// Document length at the last change: insertions grow the text,
        /// deletions shrink it (`textStorage.changeInLength` is not
        /// reliable outside text-storage callbacks).
        private var lastKnownLength = -1

        /// Re-syncs the length baseline after the text is replaced
        /// wholesale (view creation, model updates).
        func syncTextLength() {
            guard let textView else { return }
            lastKnownLength = (textView.string as NSString).length
        }

        init(_ parent: CodeEditorView) {
            self.parent = parent
            super.init()
            NotificationCenter.default.addObserver(
                self, selector: #selector(completeNow(_:)),
                name: .spiCompleteInEditor, object: nil)
        }

        /// Edit ▸ Complete (⌃Space): opens the list, or steps through it
        /// when it is already open.
        @objc private func completeNow(_ notification: Notification) {
            guard let textView,
                  textView.window != nil,
                  textView.window?.isKeyWindow == true || !requiresKeyWindow
            else { return }
            if completionPanel.isVisible {
                completionPanel.moveSelection(by: 1)
                return
            }
            guard let range = currentWordRange(in: textView) else { return }
            showCompletion(
                prefix: (textView.string as NSString).substring(with: range),
                range: range, in: textView)
        }

        /// Re-syncs the document view with the clip view's size.
        func refitEditor() {
            guard let textView, let scrollView else { return }
            CodeEditorFactory.refit(textView, in: scrollView)
            gutter?.needsDisplay = true
        }

        @objc func boundsChanged(_ notification: Notification) {
            gutter?.needsDisplay = true
            if completionPanel.isVisible, let textView,
               let rect = caretScreenRect(in: textView)
            {
                completionPanel.move(near: rect)
            }
            updateSignatureHelp()
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
            gutter?.needsDisplay = true
            let length = (textView.string as NSString).length
            let deleted = lastKnownLength >= 0 && length < lastKnownLength
            lastKnownLength = length
            if deleted {
                // Deleting must never open the completion list.
                hideCompletion()
            } else if !isApplyingModel {
                refreshCompletion(in: textView)
            }
            updateSignatureHelp()
        }

        func textDidEndEditing(_ notification: Notification) {
            hideCompletion()
            signatureHelp.hide()
        }

        func textViewDidChangeSelection(_ notification: Notification) {
            if completionPanel.isVisible, let textView,
               let range = completionPrefixRange,
               textView.selectedRange()
                   != NSRange(location: range.location + range.length, length: 0)
            {
                hideCompletion()
            }
            updateSignatureHelp()
        }

        // MARK: Key commands

        /// While the completion list is open, Up/Down move the selection
        /// and Tab/Return accept it. Return otherwise keeps the
        /// indentation, opening a level after block openers
        /// (function/if/for/while/do/else/repeat and `{`/`(`). Delete
        /// only hides the list, then always removes the character.
        func textView(_ textView: NSTextView, doCommandBy commandSelector: Selector) -> Bool {
            if completionPanel.isVisible {
                if commandSelector == #selector(NSResponder.moveUp(_:)) {
                    completionPanel.moveSelection(by: -1)
                    return true
                }
                if commandSelector == #selector(NSResponder.moveDown(_:)) {
                    completionPanel.moveSelection(by: 1)
                    return true
                }
                if commandSelector == #selector(NSResponder.insertTab(_:))
                    || commandSelector == #selector(NSResponder.insertNewline(_:))
                {
                    acceptCompletion()
                    return true
                }
                if commandSelector == #selector(NSResponder.cancelOperation(_:)) {
                    hideCompletion()
                    return true
                }
            }
            if commandSelector == #selector(NSResponder.deleteBackward(_:))
                || commandSelector == #selector(NSResponder.deleteForward(_:))
            {
                hideCompletion()
                return false // perform the deletion
            }
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
            guard let textView, parent.syntaxHighlighting else { return }
            isHighlighting = true
            CodeEditorFactory.highlightSyntax(textView)
            isHighlighting = false
        }

        // MARK: Completion

        let completionPanel = CompletionPanel()
        /// Delay before the list appears; tests shorten it.
        var completionDelay: TimeInterval = 0.15
        /// Test hook: windows do not become key in the test process, so
        /// tests relax the checks that keep panels out of background
        /// windows.
        var requiresKeyWindow = true
        private var completionPrefixRange: NSRange?
        private var pendingCompletionPrefix: String?

        /// Follows the word being typed: opens the list, filters it while
        /// it is open, or hides it. Never called for deletions.
        private func refreshCompletion(in textView: NSTextView) {
            guard let range = currentWordRange(in: textView),
                  textView.selectedRange().location == range.location + range.length
            else {
                hideCompletion()
                return
            }
            let prefix = (textView.string as NSString).substring(with: range)
            let matches = LuaCompletion.matches(prefix).filter { $0 != prefix }
            guard prefix.count >= 2, !matches.isEmpty else {
                hideCompletion()
                return
            }
            completionPrefixRange = range
            guard let rect = caretScreenRect(in: textView) else {
                hideCompletion()
                return
            }
            if completionPanel.isVisible {
                completionPanel.update(matches: matches, near: rect)
            } else {
                scheduleCompletion(in: textView)
            }
        }

        private func scheduleCompletion(in textView: NSTextView) {
            cancelPendingCompletion()
            guard let range = currentWordRange(in: textView) else { return }
            let prefix = (textView.string as NSString).substring(with: range)
            guard prefix.count >= 2 else { return }
            let matches = LuaCompletion.matches(prefix).filter { $0 != prefix }
            guard !matches.isEmpty else { return }
            pendingCompletionPrefix = prefix
            perform(
                #selector(showScheduledCompletion(_:)), with: nil,
                afterDelay: completionDelay)
        }

        /// Opens the completion list. Runs on the run loop (also under
        /// the test harness, unlike dispatch).
        @objc private func showScheduledCompletion(_ object: Any?) {
            _ = object
            let prefix = pendingCompletionPrefix
            pendingCompletionPrefix = nil
            guard let textView, let prefix,
                  textView.window != nil,
                  textView.window?.isKeyWindow == true || !requiresKeyWindow,
                  let range = currentWordRange(in: textView),
                  (textView.string as NSString).substring(with: range) == prefix
            else { return }
            showCompletion(prefix: prefix, range: range, in: textView)
        }

        private func showCompletion(
            prefix: String, range: NSRange, in textView: NSTextView
        ) {
            let matches = LuaCompletion.matches(prefix).filter { $0 != prefix }
            guard !matches.isEmpty, let rect = caretScreenRect(in: textView) else { return }
            completionPrefixRange = range
            completionPanel.show(matches: matches, near: rect)
            signatureHelp.hide() // the list owns the space below
        }

        /// Inserts the highlighted completion (Tab, Return, or a click).
        func acceptCompletion() {
            guard completionPanel.isVisible,
                  let textView, let range = completionPrefixRange,
                  let match = completionPanel.selectedMatch
            else { return }
            hideCompletion()
            textView.insertText(match, replacementRange: range)
        }

        func hideCompletion() {
            cancelPendingCompletion()
            completionPrefixRange = nil
            completionPanel.hide()
        }

        private func cancelPendingCompletion() {
            pendingCompletionPrefix = nil
            NSObject.cancelPreviousPerformRequests(
                withTarget: self, selector: #selector(showScheduledCompletion(_:)), object: nil)
        }

        private func currentWordRange(in textView: NSTextView) -> NSRange? {
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
            return NSRange(location: start, length: location - start)
        }

        // MARK: Signature help

        let signatureHelp = SignatureHelpPanel()

        /// Shows parameter help for the call the caret sits in, or hides
        /// it.
        func updateSignatureHelp() {
            guard let textView, textView.window != nil,
                  !completionPanel.isVisible,
                  textView.window?.isKeyWindow == true || !requiresKeyWindow
            else {
                signatureHelp.hide()
                return
            }
            let caret = textView.selectedRange().location
            guard let context = LuaSignatureHelp.context(at: caret, in: textView.string),
                  let rect = caretScreenRect(in: textView)
            else {
                signatureHelp.hide()
                return
            }
            signatureHelp.show(context: context, near: rect)
        }

        private func caretScreenRect(in textView: NSTextView) -> NSRect? {
            let length = (textView.string as NSString).length
            let location = min(max(textView.selectedRange().location, 0), length)
            let rect = textView.firstRect(
                forCharacterRange: NSRange(location: location, length: 0),
                actualRange: nil)
            // A caret rect has zero width but a real height.
            return rect.height > 0 ? rect : nil
        }

        // MARK: Diagnostics

        func applyDiagnostic(line: Int?) {
            guard let textView else { return }
            guard appliedDiagnosticLine != line else { return }
            appliedDiagnosticLine = line
            gutter?.errorLine = line
            isHighlighting = true
            CodeEditorFactory.applyDiagnostic(line: line, to: textView)
            isHighlighting = false
            if let line, let range = CodeEditorFactory.lineRange(line, in: textView.string) {
                textView.scrollRangeToVisible(range)
            }
        }
    }
}

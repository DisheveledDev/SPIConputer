import AppKit
import Testing

import SPIIDECore

@testable import SPIIDE

@Suite("Editor completion and parameter help")
@MainActor
struct EditorHelpTests {
    private func makeCoordinator(
        text: String
    ) -> (CodeEditorView.Coordinator, CodeEditorFactory.Editor, NSWindow) {
        let coordinator = CodeEditorView.Coordinator(
            CodeEditorView(text: .constant(text), diagnosticLine: nil))
        let editor = CodeEditorFactory.make(text: text, delegate: coordinator)
        coordinator.textView = editor.textView
        coordinator.scrollView = editor.scrollView
        coordinator.gutter = editor.gutter
        coordinator.syncTextLength()
        coordinator.requiresKeyWindow = false
        coordinator.completionDelay = 0.01

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 600, height: 300),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = editor.container
        window.makeKeyAndOrderFront(nil)
        window.layoutIfNeeded()
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        return (coordinator, editor, window)
    }

    private func putCaretAtEnd(_ textView: NSTextView) {
        textView.setSelectedRange(
            NSRange(location: (textView.string as NSString).length, length: 0))
    }

    private func type(_ text: String, into textView: NSTextView) {
        textView.insertText(text, replacementRange: textView.selectedRange())
    }

    @Test func typingOpensCompletionAndDeletingClosesIt() {
        let (coordinator, editor, window) = makeCoordinator(text: "Screen")
        defer { window.orderOut(nil) }

        putCaretAtEnd(editor.textView)
        type("O", into: editor.textView)
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        #expect(editor.textView.string == "ScreenO")
        #expect(coordinator.completionPanel.isVisible)
        #expect(coordinator.completionPanel.selectedMatch == "ScreenOut")

        editor.textView.doCommand(by: #selector(NSResponder.deleteBackward(_:)))
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        #expect(editor.textView.string == "Screen")
        #expect(!coordinator.completionPanel.isVisible)

        // Deleting keeps working and never reopens the list.
        editor.textView.doCommand(by: #selector(NSResponder.deleteBackward(_:)))
        editor.textView.doCommand(by: #selector(NSResponder.deleteBackward(_:)))
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        #expect(editor.textView.string == "Scre")
        #expect(!coordinator.completionPanel.isVisible)
    }

    @Test func completionFiltersWhileTyping() {
        let (coordinator, editor, window) = makeCoordinator(text: "Screen")
        defer { window.orderOut(nil) }

        putCaretAtEnd(editor.textView)
        type("O", into: editor.textView)
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        #expect(coordinator.completionPanel.selectedMatch == "ScreenOut")
        // A character that no longer matches hides the list.
        type("Q", into: editor.textView)
        #expect(!coordinator.completionPanel.isVisible)
    }

    @Test func completionNavigatesAndAcceptsWithTab() {
        let (coordinator, editor, window) = makeCoordinator(text: "Scree")
        defer { window.orderOut(nil) }

        putCaretAtEnd(editor.textView)
        type("n", into: editor.textView)
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        #expect(coordinator.completionPanel.isVisible)
        #expect(coordinator.completionPanel.selectedMatch == "ScreenOut")

        editor.textView.doCommand(by: #selector(NSResponder.moveDown(_:)))
        #expect(coordinator.completionPanel.selectedMatch == "ScreenAttr")

        editor.textView.doCommand(by: #selector(NSResponder.insertTab(_:)))
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        #expect(editor.textView.string == "ScreenAttr")
        #expect(!coordinator.completionPanel.isVisible)
    }

    @Test func escapeDismissesTheCompletionList() {
        let (coordinator, editor, window) = makeCoordinator(text: "Screen")
        defer { window.orderOut(nil) }

        putCaretAtEnd(editor.textView)
        type("O", into: editor.textView)
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        #expect(coordinator.completionPanel.isVisible)

        editor.textView.doCommand(by: #selector(NSResponder.cancelOperation(_:)))
        #expect(!coordinator.completionPanel.isVisible)
        #expect(editor.textView.string == "ScreenO")
    }

    @Test func menuCompleteOpensAndStepsThroughTheList() {
        let (coordinator, editor, window) = makeCoordinator(text: "Scr")
        defer { window.orderOut(nil) }

        putCaretAtEnd(editor.textView)
        NotificationCenter.default.post(name: .spiCompleteInEditor, object: nil)
        #expect(coordinator.completionPanel.isVisible)
        let first = coordinator.completionPanel.selectedMatch

        // A second invocation steps to the next entry.
        NotificationCenter.default.post(name: .spiCompleteInEditor, object: nil)
        #expect(coordinator.completionPanel.selectedMatch != first)
    }

    @Test func parameterHelpShowsForOpenCalls() {
        let (coordinator, editor, window) = makeCoordinator(text: "")
        defer { window.orderOut(nil) }

        editor.textView.string = "ScreenOut("
        putCaretAtEnd(editor.textView)
        coordinator.updateSignatureHelp()
        #expect(coordinator.signatureHelp.isVisible)
        #expect(coordinator.signatureHelp.displayText == "ScreenOut(x, y, char, [attr])")

        editor.textView.string = "local x = 1"
        putCaretAtEnd(editor.textView)
        coordinator.updateSignatureHelp()
        #expect(!coordinator.signatureHelp.isVisible)
    }

    @Test func parameterHelpTracksTheActiveParameter() {
        let signature = LuaSignatures.signature(for: "ScreenOut")!
        let text = SignatureHelpPanel.attributedText(signature: signature, active: 2)
        let ns = text.string as NSString
        let activeRange = ns.range(of: "char")
        let inactiveRange = ns.range(of: "y")

        let regular = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        let activeFont = text.attribute(.font, at: activeRange.location, effectiveRange: nil) as? NSFont
        let inactiveFont = text.attribute(.font, at: inactiveRange.location, effectiveRange: nil) as? NSFont
        #expect(activeFont != regular)
        #expect(inactiveFont == regular)
    }
}

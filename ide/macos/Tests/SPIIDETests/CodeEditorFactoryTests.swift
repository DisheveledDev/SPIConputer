import AppKit
import Testing

@testable import SPIIDE

@Suite("Editor factory")
@MainActor
struct CodeEditorFactoryTests {
    @Test func editorLaysOutWithText() {
        let editor = CodeEditorFactory.make(
            text: "line1\nline2\nline3\n", delegate: nil)

        editor.container.frame = NSRect(x: 0, y: 0, width: 640, height: 400)
        editor.container.layoutSubtreeIfNeeded()

        #expect(editor.textView.string == "line1\nline2\nline3\n")
        #expect(editor.textView.frame.width > 0)
        #expect(editor.textView.frame.height > 0)
        #expect(editor.gutter != nil)
        // The gutter sits beside the scroll view, not over it.
        #expect(editor.scrollView.frame.minX == EditorContainerView.gutterWidth)
        #expect(editor.scrollView.frame.width == 640 - EditorContainerView.gutterWidth)
    }

    @Test func gutterCanBeDisabled() {
        let editor = CodeEditorFactory.make(
            text: "x", delegate: nil, showGutter: false)
        editor.container.frame = NSRect(x: 0, y: 0, width: 400, height: 200)
        editor.container.layoutSubtreeIfNeeded()
        #expect(editor.gutter == nil)
        #expect(editor.scrollView.frame.minX == 0)
        #expect(editor.scrollView.frame.width == 400)
    }

    @Test func refitSyncsDocumentViewToClipView() {
        let text = String(repeating: "print(1)\n", count: 50)
        let editor = CodeEditorFactory.make(text: text, delegate: nil)
        editor.scrollView.frame = NSRect(x: 0, y: 0, width: 800, height: 300)
        CodeEditorFactory.refit(editor.textView, in: editor.scrollView)
        #expect(editor.textView.frame.width == editor.scrollView.contentSize.width)
        #expect(editor.textView.frame.height > 300) // 50 lines exceed the viewport
    }

    @Test func newlineIndentAfterOpeners() {
        let editor = CodeEditorFactory.make(text: "", delegate: nil)
        let textView = editor.textView

        textView.string = "function f()"
        textView.setSelectedRange(NSRange(location: 12, length: 0))
        #expect(CodeEditorFactory.indentationForNewline(in: textView) == "    ")

        textView.string = "    if x then"
        textView.setSelectedRange(NSRange(location: 13, length: 0))
        #expect(CodeEditorFactory.indentationForNewline(in: textView) == "        ")

        textView.string = "    end"
        textView.setSelectedRange(NSRange(location: 7, length: 0))
        #expect(CodeEditorFactory.indentationForNewline(in: textView) == "    ")
    }

    @Test func dedentRangeDetection() {
        let text = "        end\n"
        let dedent = CodeEditorFactory.dedentRange(
            text: text, caret: 11, typed: "d", excludingLineStart: -1)
        #expect(dedent?.lineStart == 0)
        #expect(dedent?.range == NSRange(location: 0, length: 4))

        // Only the closer's last character triggers it.
        #expect(CodeEditorFactory.dedentRange(
            text: text, caret: 11, typed: "n", excludingLineStart: -1) == nil)
        // Once per line.
        #expect(CodeEditorFactory.dedentRange(
            text: text, caret: 11, typed: "d", excludingLineStart: 0) == nil)
        // Caret must be at the end of the closer.
        #expect(CodeEditorFactory.dedentRange(
            text: text, caret: 9, typed: "d", excludingLineStart: -1) == nil)
    }
}

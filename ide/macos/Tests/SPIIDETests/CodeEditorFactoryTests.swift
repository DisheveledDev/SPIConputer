import AppKit
import Testing

@testable import SPIIDE

@Suite("Editor factory")
@MainActor
struct CodeEditorFactoryTests {
    @Test func editorLaysOutWithText() {
        let editor = CodeEditorFactory.make(
            text: "line1\nline2\nline3\n", delegate: nil)

        editor.scrollView.frame = NSRect(x: 0, y: 0, width: 640, height: 400)
        editor.scrollView.tile()

        #expect(editor.textView.string == "line1\nline2\nline3\n")
        #expect(editor.textView.frame.width > 0)
        #expect(editor.textView.frame.height > 0)
        #expect(editor.scrollView.verticalRulerView === editor.ruler)
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

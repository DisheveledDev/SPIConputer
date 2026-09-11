import AppKit
import Testing

@testable import SPIIDE

/// Temporary probe: how does AppKit's completion window react to backspace?
@Suite("Completion probe")
@MainActor
struct CompletionProbeTests {
    final class CompletionDelegate: NSObject, NSTextViewDelegate {
        var commands: [String] = []

        func textView(
            _ textView: NSTextView, completions words: [String],
            forPartialWordRange charRange: NSRange,
            indexOfSelectedItem index: UnsafeMutablePointer<Int>?
        ) -> [String] {
            ["ScreenOut", "ScreenAttr", "ScreenMode"]
        }

        func textView(
            _ textView: NSTextView, doCommandBy commandSelector: Selector
        ) -> Bool {
            commands.append(NSStringFromSelector(commandSelector))
            return false
        }
    }

    private func makeEditor(
        _ delegate: CompletionDelegate
    ) -> (CodeEditorFactory.Editor, NSWindow) {
        let editor = CodeEditorFactory.make(text: "Scr", delegate: delegate)
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 500, height: 300),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = editor.container
        window.makeKeyAndOrderFront(nil)
        editor.textView.setSelectedRange(NSRange(location: 3, length: 0))
        return (editor, window)
    }

    private func backspace(_ window: NSWindow) {
        if let event = NSEvent.keyEvent(
            with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
            windowNumber: window.windowNumber, context: nil,
            characters: "\u{8}", charactersIgnoringModifiers: "\u{8}",
            isARepeat: false, keyCode: 51)
        {
            NSApp.sendEvent(event)
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
    }

    @Test func withoutCompletionBackspaceDeletes() {
        let delegate = CompletionDelegate()
        let (editor, window) = makeEditor(delegate)
        backspace(window)
        print("PROBE A no-complete:", editor.textView.string as NSString)
        window.orderOut(nil)
    }

    @Test func withCompletionBackspaceIsEaten() {
        let delegate = CompletionDelegate()
        let (editor, window) = makeEditor(delegate)
        editor.textView.complete(nil)
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        backspace(window)
        print("PROBE B with-complete:", editor.textView.string as NSString)
        print("PROBE B commands:", delegate.commands)
        window.orderOut(nil)
    }

    @Test func cancelCompletionThenBackspace() {
        let delegate = CompletionDelegate()
        let (editor, window) = makeEditor(delegate)
        editor.textView.complete(nil)
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        editor.textView.insertCompletion(
            "Scr", forPartialWordRange: NSRange(location: 0, length: 3),
            movement: NSTextMovement.cancel.rawValue, isFinal: true)
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        print("PROBE C after cancel:", editor.textView.string as NSString)
        backspace(window)
        print("PROBE C after backspace:", editor.textView.string as NSString)
        window.orderOut(nil)
    }
}

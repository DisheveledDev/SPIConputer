import AppKit
import SwiftUI

/// NSView that captures key down/up events and reports them to the app.
final class KeySinkView: NSView {
    var onKey: ((NSEvent, _ isDown: Bool) -> Bool)?

    override var acceptsFirstResponder: Bool { true }

    override func keyDown(with event: NSEvent) {
        if onKey?(event, true) != true {
            super.keyDown(with: event)
        }
    }

    override func keyUp(with event: NSEvent) {
        _ = onKey?(event, false)
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        window?.makeFirstResponder(self)
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
    }
}

struct KeyCaptureView: NSViewRepresentable {
    let onKey: (NSEvent, _ isDown: Bool) -> Void

    func makeNSView(context: Context) -> KeySinkView {
        let view = KeySinkView()
        view.onKey = { event, isDown in
            onKey(event, isDown)
            return true
        }
        return view
    }

    func updateNSView(_ nsView: KeySinkView, context: Context) {
        nsView.onKey = { event, isDown in
            onKey(event, isDown)
            return true
        }
    }
}

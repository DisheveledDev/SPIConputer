import AppKit

/// Maps macOS key events to `input=` protocol lines.
///
/// Bare codes (e.g. `input=65`) are synthesised into down+up on the
/// board; arrow keys are sent as ESC [ A/B/C/D sequences so both dumb
/// and ANSI-aware consumers can interpret them.
enum KeyMap {
    /// Lines to send for this event (empty = nothing).
    static func lines(for event: NSEvent, isDown: Bool) -> [String] {
        // Leave Cmd shortcuts (Cmd+Q, Cmd+W, ...) to the system.
        if event.modifierFlags.contains(.command) { return [] }

        if let arrow = Self.arrowSequence(for: event.keyCode) {
            return isDown ? arrow : []
        }

        switch event.keyCode {
        case 36, 76:   // Return, keypad Enter
            return isDown ? ["input=13"] : []
        case 48:       // Tab
            return isDown ? ["input=9"] : []
        case 51:       // Delete (backspace)
            return isDown ? ["input=8"] : []
        case 117:      // Forward delete
            return isDown ? ["input=127"] : []
        case 53:       // Escape
            return isDown ? ["input=27"] : []
        default:
            break
        }

        guard isDown else { return [] }
        guard let chars = event.charactersIgnoringModifiers,
              let first = chars.first else { return [] }

        if event.modifierFlags.contains(.control), first.isLetter,
           first.isASCII {
            // Ctrl+letter -> control code 1-26.
            if let base = first.lowercased().first?.asciiValue {
                return ["input=\(base - 96)"]
            }
        }
        guard first.isASCII, let code = first.asciiValue,
              code >= 32, code <= 126 else { return [] }
        return ["input=\(code)"]
    }

    private static func arrowSequence(for keyCode: UInt16) -> [String]? {
        let seq: [String]
        switch keyCode {
        case 126: seq = ["27", "91", "65"] // up
        case 125: seq = ["27", "91", "66"] // down
        case 123: seq = ["27", "91", "68"] // left
        case 124: seq = ["27", "91", "67"] // right
        default: return nil
        }
        return seq.map { "input=\($0)" }
    }
}

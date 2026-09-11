import SwiftUI

/// Lua/snippet editor.
///
/// Default: SwiftUI's `TextEditor` (known-good rendering).
/// Settings ▸ Editor can switch to the experimental AppKit editor
/// (`CodeEditorView`: gutter, error line, completions, indent) which adds
/// syntax highlighting as a separate toggle — used to bisect the
/// "invisible text" regression on a real GUI.
struct LuaEditorView: View {
    @Environment(AppModel.self) private var model
    var diagnosticLine: Int?

    @AppStorage("experimentalEditor") private var experimentalEditor = false
    @AppStorage("syntaxHighlighting") private var syntaxHighlighting = true
    @AppStorage("editorGutter") private var editorGutter = true

    var body: some View {
        @Bindable var model = model

        if experimentalEditor {
            CodeEditorView(
                text: $model.luaText,
                diagnosticLine: diagnosticLine,
                syntaxHighlighting: syntaxHighlighting,
                gutter: editorGutter)
        } else {
            TextEditor(text: $model.luaText)
                .font(.system(.body, design: .monospaced))
                .onChange(of: model.luaText) {
                    model.scheduleSave()
                }
        }
    }
}

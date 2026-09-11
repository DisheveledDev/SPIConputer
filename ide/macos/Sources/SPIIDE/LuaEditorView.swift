import SwiftUI

/// Lua/snippet editor.
///
/// Uses SwiftUI's `TextEditor` (the known-good component); the fancier
/// AppKit editor (`CodeEditorView`, kept in the tree) is parked until the
/// "invisible text" regression can be debugged on a machine with a GUI.
/// Compile diagnostics still appear in the banner above and the log, with
/// component-mapped line numbers.
struct LuaEditorView: View {
    @Environment(AppModel.self) private var model
    var diagnosticLine: Int?

    var body: some View {
        @Bindable var model = model

        TextEditor(text: $model.luaText)
            .font(.system(.body, design: .monospaced))
            .onChange(of: model.luaText) {
                model.scheduleSave()
            }
    }
}

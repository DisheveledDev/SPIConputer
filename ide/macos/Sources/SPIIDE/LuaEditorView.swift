import SwiftUI

struct LuaEditorView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        TextEditor(text: $model.luaText)
            .font(.system(.body, design: .monospaced))
            .onChange(of: model.luaText) {
                model.scheduleSave()
            }
    }
}

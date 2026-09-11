import SwiftUI

import SPIIDECore

struct ComponentEditorView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if let component = model.selectedComponent {
            VStack(spacing: 0) {
                HStack(spacing: 8) {
                    Image(systemName: component.kind.symbolName)
                        .foregroundStyle(.secondary)
                    Text(component.name).font(.headline)
                    Text(component.kind.displayName)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(component.file)
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                        .textSelection(.enabled)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                Divider()
                editor(for: component)
            }
        } else {
            Text("Select a component, or add one with the + menu.")
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    @ViewBuilder
    private func editor(for component: ComponentRef) -> some View {
        switch component.kind {
        case .lua, .snippet:
            LuaEditorView()
        case .tiles:
            TilesEditorView()
        case .audio:
            AudioEditorView()
        }
    }
}

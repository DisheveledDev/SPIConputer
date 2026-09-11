import SwiftUI

import SPIIDECore

struct ComponentListView: View {
    @Environment(AppModel.self) private var model
    @State private var renameText = ""
    @State private var showingRename = false

    var body: some View {
        @Bindable var model = model

        List(selection: $model.selectedComponentID) {
            ForEach(model.project?.manifest.components ?? []) { component in
                ComponentRow(component: component)
                    .tag(component.id)
                    .contextMenu {
                        Button("Rename…") {
                            renameText = component.name
                            model.renameTargetID = component.id
                            showingRename = true
                        }
                        Button("Delete", role: .destructive) {
                            model.removeComponent(id: component.id)
                        }
                    }
            }
            .onMove { offsets, destination in
                model.moveComponents(from: offsets, to: destination)
            }
        }
        .onChange(of: model.selectedComponentID) {
            model.componentSelectionChanged()
        }
        .alert("Rename Component", isPresented: $showingRename) {
            TextField("Name", text: $renameText)
            Button("Rename") {
                if let id = model.renameTargetID {
                    model.renameComponent(id: id, to: renameText)
                }
                model.renameTargetID = nil
            }
            Button("Cancel", role: .cancel) { model.renameTargetID = nil }
        }
        .navigationTitle(model.project?.manifest.name ?? "Project")
        .overlay {
            if model.project == nil {
                Text("No project")
                    .foregroundStyle(.secondary)
            }
        }
    }
}

private struct ComponentRow: View {
    let component: ComponentRef

    var body: some View {
        Label {
            VStack(alignment: .leading, spacing: 1) {
                Text(component.name)
                Text(component.kind.displayName)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        } icon: {
            Image(systemName: component.kind.symbolName)
        }
    }
}

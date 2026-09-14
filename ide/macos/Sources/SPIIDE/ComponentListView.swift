import SwiftUI

import SPIIDECore

struct ComponentListView: View {
    @Environment(AppModel.self) private var model
    @State private var renameText = ""
    @State private var showingRename = false

    var body: some View {
        @Bindable var model = model

        List(selection: $model.selectedComponentID) {
            if model.project != nil {
                Button {
                    model.showingProjectSettings = true
                } label: {
                    Label("Project Settings", systemImage: "slider.horizontal.3")
                }
                .buttonStyle(.plain)
                .listRowBackground(model.showingProjectSettings ? Color.accentColor.opacity(0.15) : nil)
            }
            ForEach(model.project?.manifest.components ?? []) { component in
                ComponentRow(
                    component: component,
                    diagnostic: model.diagnostic(for: component.id))
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
            model.showingProjectSettings = false
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
    let diagnostic: CompileDiagnostic?

    var body: some View {
        Label {
            VStack(alignment: .leading, spacing: 1) {
                Text(component.name)
                    .foregroundStyle(diagnostic == nil ? Color.primary : Color.red)
                if let diagnostic {
                    Text(errorText(diagnostic))
                        .font(.caption)
                        .foregroundStyle(.red)
                        .lineLimit(1)
                } else {
                    Text(component.kind.displayName)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        } icon: {
            Image(systemName: diagnostic == nil
                ? component.kind.symbolName
                : "exclamationmark.triangle.fill")
                .foregroundStyle(diagnostic == nil ? Color.secondary : Color.red)
        }
        .help(diagnostic.map(errorText) ?? component.kind.displayName)
    }

    private func errorText(_ diagnostic: CompileDiagnostic) -> String {
        guard let line = diagnostic.location?.line else { return diagnostic.message }
        return "Line \(line): \(diagnostic.message)"
    }
}

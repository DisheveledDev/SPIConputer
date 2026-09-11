import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct NewProjectSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss

    @State private var name = "My Program"
    @State private var parent: URL?
    @State private var choosingLocation = false
    @FocusState private var nameFocused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("New Project")
                .font(.title2)

            VStack(alignment: .leading, spacing: 6) {
                Text("Name")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                TextField("Project name", text: $name)
                    .textFieldStyle(.roundedBorder)
                    .focused($nameFocused)
                    .onSubmit(create)
            }

            VStack(alignment: .leading, spacing: 6) {
                Text("Location")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                HStack {
                    Text(parent?.path ?? "Choose a folder…")
                        .foregroundStyle(parent == nil ? .secondary : .primary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    Button("Choose…") { choosingLocation = true }
                }
            }

            HStack {
                Text("Creates a project folder with a commented setup/tick/finish skeleton.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Create", action: create)
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
                    .disabled(!canCreate)
            }
        }
        .padding(20)
        .frame(width: 520)
        .defaultFocus($nameFocused, true)
        .onAppear {
            // Make sure the sheet's window is key before focusing the field;
            // focusing immediately can be lost while the sheet animates in.
            NSApp.activate(ignoringOtherApps: true)
            Task { @MainActor in
                try? await Task.sleep(for: .milliseconds(60))
                nameFocused = true
            }
        }
        .fileImporter(
            isPresented: $choosingLocation,
            allowedContentTypes: [.folder]
        ) { result in
            if case .success(let url) = result {
                parent = url
            }
        }
    }

    private var canCreate: Bool {
        parent != nil && !name.trimmingCharacters(in: .whitespaces).isEmpty
    }

    private func create() {
        guard canCreate, let parent else { return }
        model.createProject(named: name, in: parent)
        dismiss()
    }
}

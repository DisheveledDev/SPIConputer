import SwiftUI
import UniformTypeIdentifiers

struct SettingsView: View {
    @Environment(AppModel.self) private var model
    @State private var choosingSimulator = false
    @AppStorage("experimentalEditor") private var experimentalEditor = false
    @AppStorage("syntaxHighlighting") private var syntaxHighlighting = true
    @AppStorage("editorGutter") private var editorGutter = true

    var body: some View {
        @Bindable var model = model

        Form {
            Section("Editor") {
                Toggle("Use experimental AppKit editor", isOn: $experimentalEditor)
                Text("""
                    Adds a line-number gutter, error-line highlight, \
                    completions and auto-indent. Off by default while the \
                    rendering issue is being investigated.
                    """)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Toggle("Syntax highlighting", isOn: $syntaxHighlighting)
                    .disabled(!experimentalEditor)
                Toggle("Show line-number gutter", isOn: $editorGutter)
                    .disabled(!experimentalEditor)
            }
            Section("Simulator") {
                LabeledContent("Path") {
                    HStack {
                        TextField("Auto-detect", text: $model.simulatorPathPreference)
                            .frame(width: 360)
                        Button("Choose…") { choosingSimulator = true }
                        Button("Auto-detect") { model.simulatorPathPreference = "" }
                    }
                }
                LabeledContent("Detected") {
                    Text(model.simulatorURL?.path ?? "not found — build it, or set a path above")
                        .foregroundStyle(model.simulatorURL == nil ? .secondary : .primary)
                        .textSelection(.enabled)
                }
                Text("""
                    Build the simulator with:
                    cmake -S simulator -B simulator/build && cmake --build simulator/build
                    """)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .padding()
        .frame(width: 620)
        .fileImporter(
            isPresented: $choosingSimulator,
            allowedContentTypes: [.executable]
        ) { result in
            if case .success(let url) = result {
                model.simulatorPathPreference = url.path
            }
        }
    }
}

import SwiftUI
import UniformTypeIdentifiers

struct SettingsView: View {
    @Environment(AppModel.self) private var model
    @State private var choosingSimulator = false

    var body: some View {
        @Bindable var model = model

        Form {
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

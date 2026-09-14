import SwiftUI

import SPIIDECore

struct ProjectSettingsView: View {
    @Environment(AppModel.self) private var model
    @State private var name = ""
    @State private var version = ""
    @State private var description = ""
    @State private var interactive = true
    @State private var outputKind: ProjectOutputKind = .app
    @State private var video = true
    @State private var audio = true
    @State private var iconFile = ""
    @State private var sdks: Set<String> = []

    var body: some View {
        Form {
            Section("Application") {
                TextField("Name", text: $name)
                TextField("Version", text: $version)
                TextField("Description", text: $description)
                    .help("One line shown by the shell's APPS picker (about 34 characters fit)")
                TextField("Icon file", text: $iconFile)
                    .help("Optional project-relative icon file")
            }
            Section("Frameworks") {
                ForEach(SDKLibrary.available) { sdk in
                    Toggle(isOn: Binding(
                        get: { sdks.contains(sdk.id) },
                        set: { on in if on { sdks.insert(sdk.id) } else { sdks.remove(sdk.id) } }
                    )) {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("\(sdk.title)  (\(sdk.namespaces.joined(separator: ", ")))")
                            Text(sdk.summary)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                Text("Selected frameworks are injected read-only ahead of your components; only the functions your code uses are kept in the built program.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Section("Runtime") {
                Picker("Output", selection: $outputKind) {
                    ForEach(ProjectOutputKind.allCases, id: \.self) { kind in
                        Text(kind.displayName).tag(kind)
                    }
                }
                Toggle("Interactive application", isOn: $interactive)
                Toggle("Requires video state", isOn: $video)
                    .disabled(!interactive)
                Toggle("Requires audio state", isOn: $audio)
                    .disabled(!interactive)
                Text(outputKind == .app ? "Output is an installable .app bundle." : "Output is a PRG-only system program.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Section("Output") {
                LabeledContent("Source", value: model.project?.programFileName ?? "")
                LabeledContent("Bundle", value: model.project?.appBundleURL.lastPathComponent ?? "")
                LabeledContent("Metadata", value: "app.json")
                LabeledContent("Entry point", value: "app.prg")
            }
            HStack {
                Spacer()
                Button("Save Settings") {
                    model.updateProjectSettings(
                        name: name,
                        version: version,
                        description: description,
                        interactive: interactive,
                        outputKind: outputKind,
                        requiresVideo: video,
                        requiresAudio: audio,
                        iconFile: iconFile,
                        sdks: Array(sdks))
                }
                .buttonStyle(.borderedProminent)
            }
        }
        .formStyle(.grouped)
        .padding()
        .navigationTitle("Project Settings")
        .onAppear { load() }
        .onChange(of: model.project?.manifest.name) { load() }
    }

    private func load() {
        guard let manifest = model.project?.manifest else { return }
        name = manifest.name
        version = manifest.version
        description = manifest.description
        interactive = manifest.interactive
        outputKind = manifest.outputKind
        video = manifest.requiresVideo
        audio = manifest.requiresAudio
        iconFile = manifest.iconFile ?? ""
        sdks = Set(manifest.sdks)
    }
}

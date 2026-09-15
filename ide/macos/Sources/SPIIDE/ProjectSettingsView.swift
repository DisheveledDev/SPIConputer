import SwiftUI

import SPIIDECore

struct ProjectSettingsView: View {
    @Environment(AppModel.self) private var model
    @State private var name = ""
    @State private var version = ""
    @State private var description = ""
    @State private var kind: ProjectKind = .application
    @State private var installDirectory = ""
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
            Section("Kind") {
                Picker("Kind", selection: $kind) {
                    ForEach(ProjectKind.allCases, id: \.self) { kind in
                        Text(kind.displayName).tag(kind)
                    }
                }
                Text(kind.summary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Toggle("Requires video state", isOn: $video)
                    .disabled(!kind.interactive)
                Toggle("Requires audio state", isOn: $audio)
                    .disabled(!kind.interactive)
                TextField("Install folder on the card", text: $installDirectory,
                          prompt: Text(kind.installDirectory))
                    .help("Leave empty for the kind's default (\(kind.installDirectory)/); the boot program and the shell use core")
            }
            Section("Output") {
                LabeledContent("Build folder", value: "build/")
                LabeledContent("Source", value: model.project?.programFileName ?? "")
                LabeledContent("Program", value: model.project?.prgFileName ?? "")
                if let bundle = model.project?.bundleName {
                    LabeledContent("Bundle", value: "\(bundle)/ (app.prg, app.json, resources/)")
                }
                LabeledContent("Installs to",
                               value: "\(installDirectory.isEmpty ? kind.installDirectory : installDirectory)/")
            }
            HStack {
                Spacer()
                Button("Save Settings") {
                    model.updateProjectSettings(
                        name: name,
                        version: version,
                        description: description,
                        kind: kind,
                        installDirectory: installDirectory,
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
        kind = manifest.kind
        installDirectory = manifest.installDirectory ?? ""
        video = manifest.requiresVideo
        audio = manifest.requiresAudio
        iconFile = manifest.iconFile ?? ""
        sdks = Set(manifest.sdks)
    }
}

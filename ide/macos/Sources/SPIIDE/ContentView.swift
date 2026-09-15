import SwiftUI
import UniformTypeIdentifiers

import SPIIDECore

struct ContentView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model

        NavigationSplitView {
            ComponentListView()
                .navigationSplitViewColumnWidth(min: 220, ideal: 260)
        } detail: {
            VStack(spacing: 0) {
                if model.project == nil {
                    WelcomeView()
                } else if model.showingProjectSettings {
                    ProjectSettingsView()
                } else {
                    ComponentEditorView()
                }
                Divider()
                ConsoleView()
            }
            .navigationTitle(model.project?.manifest.name ?? "SPIComputer IDE")
            .toolbar { toolbarContent }
        }
        .sheet(isPresented: $model.showingNewProject) {
            NewProjectSheet()
                .environment(model)
        }
        .fileImporter(
            isPresented: $model.showingOpenPanel,
            allowedContentTypes: [.folder]
        ) { result in
            if case .success(let url) = result {
                model.openProject(at: url)
            }
        }
        .alert(
            "Something went wrong",
            isPresented: Binding(
                get: { model.errorMessage != nil },
                set: { if !$0 { model.errorMessage = nil } })
        ) {
            Button("OK") { model.errorMessage = nil }
        } message: {
            Text(model.errorMessage ?? "")
        }
    }

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        @Bindable var model = model

        ToolbarItemGroup {
            Button("New Project", systemImage: "plus.rectangle.on.folder") {
                model.showingNewProject = true
            }
            Button("Open Project", systemImage: "folder") {
                model.showingOpenPanel = true
            }
            if model.project != nil {
                Menu("Add Component", systemImage: "plus") {
                    ForEach(ComponentKind.allCases, id: \.self) { kind in
                        Button(kind.displayName) { model.addComponent(kind: kind) }
                    }
                }
                Button("Build", systemImage: "hammer") { model.build() }
                    .help("Build the project to \(model.project?.buildProductURL.lastPathComponent ?? "")")
                if model.isRunning {
                    Button("Stop", systemImage: "stop.fill") { model.stop() }
                } else {
                    Button("Run", systemImage: "play.fill") {
                        Task { await model.run() }
                    }
                    .help("Build and run in the simulator")
                }
                Button("Install", systemImage: "square.and.arrow.down") {
                    model.install()
                }
                .help("Build, then copy the product into the SD card image (Settings)")
                Button("Reveal Build Output", systemImage: "folder") {
                    model.revealBuildOutput()
                }
            }
        }
    }
}

struct WelcomeView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "chevron.left.forwardslash.chevron.right")
                .font(.system(size: 44))
                .foregroundStyle(.secondary)
            Text("No project open")
                .font(.title2)
            Text("Create a project to generate a Lua program for the SPIComputer OS.")
                .foregroundStyle(.secondary)
            HStack {
                Button("New Project…") { model.showingNewProject = true }
                    .buttonStyle(.borderedProminent)
                Button("Open Project…") { model.showingOpenPanel = true }
            }
            if !model.recentProjects.isEmpty {
                RecentProjectsList()
                    .padding(.top, 8)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
        .onAppear { model.refreshRecentProjects() }
    }
}

/// The welcome screen's recently opened projects: click to open, right
/// click to remove or reveal.
struct RecentProjectsList: View {
    @Environment(AppModel.self) private var model

    /// "2 hours ago", "yesterday", ...
    private static let relativeFormatter: RelativeDateTimeFormatter = {
        let formatter = RelativeDateTimeFormatter()
        formatter.unitsStyle = .full
        return formatter
    }()

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("Recent Projects")
                    .font(.headline)
                Spacer()
                Button("Clear") { model.clearRecentProjects() }
                    .buttonStyle(.plain)
                    .foregroundStyle(.secondary)
                    .font(.caption)
            }
            ForEach(model.recentProjects) { entry in
                Button {
                    model.openRecentProject(entry)
                } label: {
                    HStack(spacing: 10) {
                        Image(systemName: "folder")
                            .foregroundStyle(.secondary)
                        VStack(alignment: .leading, spacing: 1) {
                            Text(entry.name)
                            Text(entry.displayPath)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        }
                        Spacer()
                        Text(Self.relativeFormatter.localizedString(for: entry.lastOpened, relativeTo: Date()))
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
                    .padding(.vertical, 4)
                    .padding(.horizontal, 8)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 6))
                .contextMenu {
                    Button("Reveal in Finder") {
                        NSWorkspace.shared.activateFileViewerSelecting([entry.url])
                    }
                    Button("Remove from Recent Projects") {
                        model.removeRecentProject(entry)
                    }
                }
                .help(entry.path)
            }
        }
        .frame(maxWidth: 480)
    }
}

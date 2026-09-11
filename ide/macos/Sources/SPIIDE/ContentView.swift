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
                    Button("Run", systemImage: "play.fill") { model.run() }
                        .help("Build and run in the simulator")
                }
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
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding()
    }
}

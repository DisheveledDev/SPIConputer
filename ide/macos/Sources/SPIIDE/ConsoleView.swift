import SwiftUI

struct ConsoleView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Text("Build & Run")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button("Reveal Output", systemImage: "folder") {
                    model.revealBuildOutput()
                }
                .controlSize(.small)
                .disabled(model.lastBuildURL == nil)
                Button("Clear", systemImage: "trash") {
                    model.clearConsole()
                }
                .controlSize(.small)
                .labelStyle(.iconOnly)
                .help("Clear the log")
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            Divider()
            ScrollView {
                Text(model.console.isEmpty ? "No output yet." : model.console)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(model.console.isEmpty ? .secondary : .primary)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
            }
        }
        .frame(height: 140)
    }
}

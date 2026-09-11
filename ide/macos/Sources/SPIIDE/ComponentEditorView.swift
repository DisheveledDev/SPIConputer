import SwiftUI

import SPIIDECore

struct ComponentEditorView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        if let component = model.selectedComponent {
            VStack(spacing: 0) {
                HStack(spacing: 8) {
                    Image(systemName: component.kind.symbolName)
                        .foregroundStyle(.secondary)
                    Text(component.name).font(.headline)
                    Text(component.kind.displayName)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(component.file)
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                        .textSelection(.enabled)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                Divider()
                if let diagnostic = diagnostic(for: component) {
                    CompileErrorBanner(diagnostic: diagnostic)
                    Divider()
                } else if let reason = model.compileCheckUnavailableReason {
                    CompileUnavailableBanner(reason: reason)
                    Divider()
                }
                editor(for: component)
            }
        } else {
            Text("Select a component, or add one with the + menu.")
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    /// The current diagnostic when it belongs to this component.
    private func diagnostic(for component: ComponentRef) -> CompileDiagnostic? {
        guard let diagnostic = model.compileDiagnostic,
              diagnostic.location?.componentID == component.id
        else { return nil }
        return diagnostic
    }

    @ViewBuilder
    private func editor(for component: ComponentRef) -> some View {
        switch component.kind {
        case .lua, .snippet:
            LuaEditorView(diagnosticLine: diagnostic(for: component)?.location?.line)
        case .tiles:
            TilesEditorView()
        case .audio:
            AudioEditorView()
        }
    }
}

/// Red banner above the editor for a compile error.
struct CompileErrorBanner: View {
    let diagnostic: CompileDiagnostic

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.red)
            if let line = diagnostic.location?.line {
                Text("Line \(line):")
                    .font(.callout.weight(.semibold))
            }
            Text(diagnostic.message)
                .font(.callout)
                .textSelection(.enabled)
            Spacer()
            if let generated = diagnostic.generatedLine {
                Text("generated line \(generated)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.red.opacity(0.12))
    }
}

/// Orange banner shown while compile checking cannot run at all.
struct CompileUnavailableBanner: View {
    let reason: String

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: "info.circle")
                .foregroundStyle(.orange)
            Text("Compile checks unavailable: \(reason)")
                .font(.callout)
                .textSelection(.enabled)
            Spacer()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 4)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.orange.opacity(0.10))
    }
}

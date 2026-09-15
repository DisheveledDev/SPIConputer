import SwiftUI

import SPIIDECore

/// Shown above whichever component is being edited when the running
/// program hit an error. Never moves the user by itself: "Show" selects
/// the component the error belongs to, "Go to Line" places the caret.
struct RuntimeErrorBanner: View {
    let diagnostic: CompileDiagnostic
    /// Name of the component the error maps to, when it is not the one
    /// being edited.
    var otherComponentName: String? = nil
    var showComponent: () -> Void = {}

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: "exclamationmark.octagon.fill")
                .foregroundStyle(.red)
            if let other = otherComponentName {
                Text("Runtime error in \(other)\(diagnostic.location.map { ", line \($0.line)" } ?? ""):")
                    .font(.callout.weight(.semibold))
            } else if let line = diagnostic.location?.line {
                Text("Runtime error, line \(line):")
                    .font(.callout.weight(.semibold))
            } else {
                Text("Runtime error:")
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
            if otherComponentName != nil {
                Button("Show", action: showComponent)
                    .controlSize(.small)
            } else if let line = diagnostic.location?.line {
                Button("Go to Line") {
                    NotificationCenter.default.post(
                        name: .spiRevealLineInEditor, object: nil, userInfo: ["line": line])
                }
                .controlSize(.small)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.red.opacity(0.16))
    }
}

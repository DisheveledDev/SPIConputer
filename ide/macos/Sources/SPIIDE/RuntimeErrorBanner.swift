import SwiftUI

import SPIIDECore

struct RuntimeErrorBanner: View {
    let diagnostic: CompileDiagnostic

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: "exclamationmark.octagon.fill")
                .foregroundStyle(.red)
            if let line = diagnostic.location?.line {
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
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.red.opacity(0.16))
    }
}

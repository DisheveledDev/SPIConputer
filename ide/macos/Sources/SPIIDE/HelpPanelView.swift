import SwiftUI

import SPIIDECore

/// The help column to the right of the editor: a search field, the
/// current topic (signature, summary, parameters, returns, description,
/// example, see-also links) and a browsable index when nothing is
/// selected. Stays put until closed; right-clicking a name in the editor
/// or following a link changes the topic.
struct HelpPanelView: View {
    @Environment(AppModel.self) private var model
    @State private var query = ""

    var body: some View {
        @Bindable var model = model
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 6) {
                Button {
                    model.helpBack()
                } label: {
                    Image(systemName: "chevron.left")
                }
                .buttonStyle(.plain)
                .disabled(model.helpHistory.isEmpty)
                .help("Back")
                TextField("Search help", text: $query)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit {
                        if let first = HelpLibrary.search(query).first {
                            model.showHelp(for: first.name)
                            query = ""
                        }
                    }
                Button {
                    model.showingHelp = false
                } label: {
                    Image(systemName: "xmark")
                }
                .buttonStyle(.plain)
                .help("Close the help panel")
            }
            .padding(8)
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    if !query.isEmpty {
                        results
                    } else if let topic = model.helpTopic, let entry = HelpLibrary.entry(named: topic) {
                        HelpEntryView(entry: entry)
                    } else if let topic = model.helpTopic {
                        Text("No help for “\(topic)”.")
                            .foregroundStyle(.secondary)
                        Text("Right-click a function, module or keyword in the editor, or search above.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        index
                    } else {
                        index
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(12)
            }
        }
        .frame(minWidth: 280, idealWidth: 340, maxWidth: 480)
    }

    private var results: some View {
        VStack(alignment: .leading, spacing: 4) {
            let hits = HelpLibrary.search(query)
            if hits.isEmpty {
                Text("Nothing matches “\(query)”.").foregroundStyle(.secondary)
            }
            ForEach(hits) { entry in
                Button {
                    model.showHelp(for: entry.name)
                    query = ""
                } label: {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(entry.signature).font(.system(.body, design: .monospaced))
                        Text(entry.summary).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .padding(.vertical, 2)
            }
        }
    }

    private var index: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Help").font(.title3.weight(.semibold))
            Text("Right-click a name in the editor for its help, or pick a group.")
                .font(.caption)
                .foregroundStyle(.secondary)
            ForEach(HelpLibrary.groups, id: \.title) { group in
                DisclosureGroup(group.title) {
                    ForEach(group.entries) { entry in
                        Button(entry.name) { model.showHelp(for: entry.name) }
                            .buttonStyle(.plain)
                            .font(.system(.body, design: .monospaced))
                            .foregroundStyle(Color.accentColor)
                    }
                }
            }
        }
    }
}

/// One topic, laid out top to bottom.
struct HelpEntryView: View {
    @Environment(AppModel.self) private var model
    let entry: HelpEntry

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text(entry.signature)
                    .font(.system(.title3, design: .monospaced).weight(.semibold))
                    .textSelection(.enabled)
                Spacer()
                Text(badge)
                    .font(.caption2.weight(.semibold))
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(.quaternary, in: Capsule())
            }
            if !entry.summary.isEmpty {
                Text(entry.summary).font(.body.weight(.medium)).textSelection(.enabled)
            }
            if !entry.parameters.isEmpty {
                section("Parameters") {
                    ForEach(entry.parameters, id: \.name) { parameter in
                        HStack(alignment: .firstTextBaseline, spacing: 8) {
                            Text(parameter.name)
                                .font(.system(.body, design: .monospaced))
                                .frame(minWidth: 70, alignment: .leading)
                            VStack(alignment: .leading, spacing: 1) {
                                HStack(spacing: 6) {
                                    if !parameter.type.isEmpty {
                                        Text(parameter.type).font(.caption).foregroundStyle(.secondary)
                                    }
                                    if parameter.optional {
                                        Text("optional").font(.caption2).foregroundStyle(.tertiary)
                                    }
                                }
                                if !parameter.description.isEmpty {
                                    Text(parameter.description).font(.callout)
                                }
                            }
                        }
                    }
                }
            }
            if !entry.returns.isEmpty {
                section("Returns") { Text(entry.returns).font(.callout).textSelection(.enabled) }
            }
            if !entry.description.isEmpty {
                section("About") { Text(entry.description).font(.callout).textSelection(.enabled) }
            }
            if !entry.example.isEmpty {
                section("Example") {
                    Text(entry.example)
                        .font(.system(.callout, design: .monospaced))
                        .textSelection(.enabled)
                        .padding(8)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 6))
                }
            }
            let members = HelpLibrary.members(of: entry.name)
            if entry.kind == "namespace" || entry.kind == "module", !members.isEmpty {
                section("Members") {
                    ForEach(members) { member in
                        Button {
                            model.showHelp(for: member.name)
                        } label: {
                            HStack(alignment: .firstTextBaseline, spacing: 8) {
                                Text(member.name.split(separator: ".").last.map(String.init) ?? member.name)
                                    .font(.system(.callout, design: .monospaced))
                                    .foregroundStyle(Color.accentColor)
                                Text(member.summary).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            }
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            if !entry.seeAlso.isEmpty {
                section("See also") {
                    FlowLinks(names: entry.seeAlso) { model.showHelp(for: $0) }
                }
            }
        }
    }

    private var badge: String {
        switch entry.source {
        case "lua": "Lua 5.5"
        case "os": "OS API"
        case "sdk": "\(entry.framework ?? "framework") SDK"
        default: entry.source
        }
    }

    @ViewBuilder
    private func section<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title.uppercased())
                .font(.caption2.weight(.semibold))
                .foregroundStyle(.secondary)
            content()
        }
    }
}

/// A wrapping row of link buttons.
private struct FlowLinks: View {
    let names: [String]
    let open: (String) -> Void

    var body: some View {
        // Simple wrapping: one link per line keeps layout predictable at
        // narrow widths and the lists are short.
        VStack(alignment: .leading, spacing: 2) {
            ForEach(names, id: \.self) { name in
                Button(name) { open(name) }
                    .buttonStyle(.plain)
                    .font(.system(.callout, design: .monospaced))
                    .foregroundStyle(Color.accentColor)
            }
        }
    }
}

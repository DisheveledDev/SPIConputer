import AppKit
import SwiftUI
import TerminalC

struct ContentView: View {
    @StateObject private var model = TerminalModel()
    @StateObject private var logger = RawLogger()

    private let session = SerialSession()

    @State private var ports: [String] = SerialPort.availablePorts()
    @State private var selectedPort = ""
    @State private var status = "not connected"

    var body: some View {
        VStack(spacing: 0) {
            DisplayView(model: model)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .overlay(KeyCaptureView(onKey: handleKey))
                .background(Color.black)

            Divider()

            HStack(spacing: 8) {
                Picker("Port", selection: $selectedPort) {
                    Text("(none)").tag("")
                    ForEach(ports, id: \.self) { Text($0) }
                }
                .frame(width: 220)

                Button("Refresh") { ports = SerialPort.availablePorts() }

                Button(model.connected ? "Disconnect" : "Connect") {
                    toggleConnection()
                }
                .disabled(!model.connected && selectedPort.isEmpty)

                Toggle("Log bytes", isOn: $logger.enabled)

                Text(status)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)

                Spacer()
            }
            .padding(8)
        }
        .onAppear {
            if selectedPort.isEmpty { selectedPort = ports.first ?? "" }
            session.onRecord = { record in model.apply(record) }
            session.onStatus = { status = $0 }
            session.onRawBytes = { data in logger.append(data) }
        }
    }

    private func toggleConnection() {
        if model.connected {
            session.disconnect()
            model.setConnected(false)
            status = "not connected"
        } else if session.connect(to: selectedPort) {
            model.setConnected(true)
        }
    }

    private func handleKey(_ event: NSEvent, _ isDown: Bool) {
        guard model.connected else { return }
        session.send(KeyMap.lines(for: event, isDown: isDown))
    }
}

/// Optional raw-byte log; writes both directions to a user-chosen file.
final class RawLogger: ObservableObject {
    @Published var enabled = false {
        didSet {
            if enabled {
                start()
            } else {
                try? handle?.close()
                handle = nil
                path = nil
            }
        }
    }

    private var handle: FileHandle?
    private var path: String?

    private func start() {
        guard path == nil else { return }
        let panel = NSSavePanel()
        panel.title = "Save raw byte log"
        panel.nameFieldStringValue = "spiterminal.log"
        if panel.runModal() == .OK, let url = panel.url {
            FileManager.default.createFile(atPath: url.path, contents: nil)
            handle = try? FileHandle(forWritingTo: url)
            path = url.path
        } else {
            enabled = false // user cancelled
        }
    }

    func append(_ data: Data) {
        guard enabled else { return }
        handle?.write(data)
    }
}

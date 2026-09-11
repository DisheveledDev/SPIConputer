import Foundation
import TerminalC

/// Owns the serial port and reader thread; feeds decoded protocol
/// records to the UI on the main queue.
final class SerialSession {
    private let protocolSession = ProtocolSession()
    private var port: SerialPort?
    private var thread: Thread?
    private(set) var isRunning = false

    var onRecord: ((proto_record_t) -> Void)?
    var onRawBytes: ((Data) -> Void)?
    var onStatus: ((String) -> Void)?

    @discardableResult
    func connect(to path: String) -> Bool {
        disconnect()

        let port = SerialPort(path: path)
        do {
            try port.open()
        } catch {
            onStatus?(error.localizedDescription)
            return false
        }
        self.port = port
        isRunning = true
        onStatus?("connected to \(path)")

        let session = protocolSession
        let thread = Thread { [weak self] in
            var buffer = [UInt8](repeating: 0, count: 4096)
            while self?.isRunning == true, port.isOpen {
                let n = port.read(into: &buffer, capacity: buffer.count)
                if n > 0 {
                    let data = Data(bytes: buffer, count: n)
                    self?.onRawBytes?(data)
                    session.feed(data) { [weak self] record in
                        self?.onRecord?(record)
                    }
                } else if n == 0 {
                    usleep(10_000) // no data yet
                } else {
                    break // -1: port error or closed
                }
            }
            DispatchQueue.main.async { [weak self] in
                self?.isRunning = false
                self?.port = nil
                self?.onStatus?("disconnected")
            }
        }
        thread.name = "SPITerminal serial reader"
        self.thread = thread
        thread.start()
        return true
    }

    func disconnect() {
        isRunning = false
        port?.close()
        port = nil
        thread = nil
    }

    /// Sends one protocol line (e.g. "input=65").
    func send(_ line: String) {
        guard let data = (line + "\n").data(using: .utf8) else { return }
        port?.write(data)
        onRawBytes?(data)
    }

    func send(_ lines: [String]) {
        for line in lines { send(line) }
    }
}

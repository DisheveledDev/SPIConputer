import Foundation
import TerminalC

/// Wraps the C parser (protocol.c) with a convenient incremental feeder.
final class ProtocolSession {
    private var parser = proto_parser_t()

    init() {
        proto_parser_init(&parser)
    }

    /// Feeds a chunk; decoded records are delivered on the main queue.
    func feed(_ data: Data, onRecord: @escaping (proto_record_t) -> Void) {
        data.withUnsafeBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else {
                return
            }
            var offset = 0
            var record = proto_record_t()
            while offset < data.count {
                let used = proto_parse(&parser, base + offset,
                                       data.count - offset, &record)
                if used > 0 {
                    let rec = record
                    DispatchQueue.main.async { onRecord(rec) }
                    offset += used
                } else {
                    offset = data.count // whole remainder consumed
                }
            }
        }
    }
}

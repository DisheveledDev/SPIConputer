import Foundation

public enum PrgCompiler {
    public struct Outcome: Sendable {
        public let outputURL: URL?
        public let error: String?
        public let unavailableReason: String?

        public init(outputURL: URL?, error: String?, unavailableReason: String?) {
            self.outputURL = outputURL
            self.error = error
            self.unavailableReason = unavailableReason
        }
    }

    public static func compile(source: URL, output: URL, simulator: URL) -> Outcome {
        let process = Process()
        process.executableURL = simulator
        process.arguments = ["--compile", source.path, output.path]
        let stdout = Pipe()
        let stderr = Pipe()
        process.standardOutput = stdout
        process.standardError = stderr

        do {
            try process.run()
        } catch {
            return Outcome(outputURL: nil, error: nil, unavailableReason: error.localizedDescription)
        }

        let stdoutData = stdout.fileHandleForReading.readDataToEndOfFile()
        let stderrData = stderr.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()

        guard process.terminationStatus == 0 else {
            let message = String(data: stderrData, encoding: .utf8)
                ?? String(data: stdoutData, encoding: .utf8)
                ?? "bytecode compilation failed"
            return Outcome(outputURL: nil, error: message.trimmingCharacters(in: .whitespacesAndNewlines), unavailableReason: nil)
        }
        return Outcome(outputURL: output, error: nil, unavailableReason: nil)
    }
}

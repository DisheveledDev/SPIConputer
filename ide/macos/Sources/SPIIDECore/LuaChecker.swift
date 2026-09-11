import Foundation

/// Result of a compile (syntax) check.
public struct LuaCheckResult: Sendable, Equatable {
    /// 1-based line in the generated program, when Lua reported one.
    public let line: Int?
    /// Line named inside the message ("at line N"), e.g. the opener of an
    /// unclosed function, when Lua supplied one.
    public let contextLine: Int?
    public let message: String

    public init(line: Int?, contextLine: Int?, message: String) {
        self.line = line
        self.contextLine = contextLine
        self.message = message
    }
}

/// Runs the OS's own Lua build (inside the simulator binary) to compile a
/// generated program, so what the IDE checks is exactly what the OS will
/// load. Requires a built simulator; returns nil when it is missing.
public enum LuaChecker {
    public struct Outcome: Sendable {
        /// nil when the source compiled cleanly.
        public let error: LuaCheckResult?
        /// Set when the check could not run at all.
        public let unavailableReason: String?
    }

    public static func check(source: String, simulator: URL) -> Outcome {
        let temp = FileManager.default.temporaryDirectory
            .appendingPathComponent("spiide-check-\(UUID().uuidString).lua")
        do {
            try Data(source.utf8).write(to: temp, options: .atomic)
        } catch {
            return Outcome(error: nil, unavailableReason: error.localizedDescription)
        }
        defer { try? FileManager.default.removeItem(at: temp) }

        let process = Process()
        process.executableURL = simulator
        process.arguments = ["--check", temp.path]
        let out = Pipe()
        let err = Pipe()
        process.standardOutput = out
        process.standardError = err
        do {
            try process.run()
        } catch {
            return Outcome(error: nil, unavailableReason: error.localizedDescription)
        }
        let outData = out.fileHandleForReading.readDataToEndOfFile()
        let errData = err.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()

        if process.terminationStatus == 0 {
            return Outcome(error: nil, unavailableReason: nil)
        }
        let raw = String(data: errData, encoding: .utf8).flatMap { $0.isEmpty ? nil : $0 }
            ?? String(data: outData, encoding: .utf8)
            ?? "compile failed"
        let parsed = LuaErrorParser.parse(raw)
        return Outcome(
            error: LuaCheckResult(
                line: parsed.line, contextLine: parsed.contextLine,
                message: parsed.message),
            unavailableReason: nil)
    }
}

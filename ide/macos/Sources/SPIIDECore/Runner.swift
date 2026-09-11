import Foundation

/// Where a run puts the simulator's virtual SD card.
public struct RunSession: Sendable, Equatable {
    public let sdcardURL: URL
    public let programURL: URL

    public init(sdcardURL: URL, programURL: URL) {
        self.sdcardURL = sdcardURL
        self.programURL = programURL
    }
}

/// Prepares a run folder: an SD card containing the built program, which
/// the simulator boots directly.
public enum Runner {
    public static func runDirectory(for project: Project) -> URL {
        project.outputDirectoryURL.appendingPathComponent("run")
    }

    public static func prepare(project: Project, build: BuildProduct) throws -> RunSession {
        let sdcard = runDirectory(for: project).appendingPathComponent("sdcard")
        try FileManager.default.createDirectory(at: sdcard, withIntermediateDirectories: true)

        let programURL = sdcard.appendingPathComponent(project.programFileName)
        try Data(build.lua.utf8).write(to: programURL, options: .atomic)

        return RunSession(sdcardURL: sdcard, programURL: programURL)
    }

    /// Command-line arguments for the simulator.
    public static func simulatorArguments(for session: RunSession) -> [String] {
        [
            "--sdcard", session.sdcardURL.path,
            "--boot", session.programURL.lastPathComponent,
        ]
    }

    /// Where the IDE looks for the simulator by default, given the app's
    /// executable location (inside `ide/macos/.build/...`).
    public static func defaultSimulatorSearchStarts(executable: URL?, cwd: URL?) -> [URL] {
        var starts: [URL] = []
        if let executable {
            starts.append(executable)
        }
        if let cwd {
            starts.append(cwd)
        }
        return starts
    }
}

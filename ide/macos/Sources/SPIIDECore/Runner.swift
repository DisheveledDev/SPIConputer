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
        let core = sdcard.appendingPathComponent("core")
        let apps = sdcard.appendingPathComponent("apps")
        let data = sdcard.appendingPathComponent("data")
        try FileManager.default.createDirectory(at: core, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: apps, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: data, withIntermediateDirectories: true)

        let installDirectory = project.manifest.outputKind == .prg ? core : apps
        let sourceURL = installDirectory.appendingPathComponent(project.programFileName)
        try Data(build.lua.utf8).write(to: sourceURL, options: .atomic)

        let programURL: URL
        if project.manifest.outputKind == .app,
           FileManager.default.fileExists(atPath: project.appBundleURL.path) {
            let appURL = apps.appendingPathComponent(project.appBundleURL.lastPathComponent)
            try? FileManager.default.removeItem(at: appURL)
            try FileManager.default.copyItem(at: project.appBundleURL, to: appURL)
            programURL = appURL.appendingPathComponent("app.prg")
        } else if FileManager.default.fileExists(atPath: project.prgProductURL.path) {
            let compiledURL = installDirectory.appendingPathComponent(project.prgFileName)
            try? FileManager.default.removeItem(at: compiledURL)
            try FileManager.default.copyItem(at: project.prgProductURL, to: compiledURL)
            programURL = compiledURL
        } else {
            programURL = sourceURL
        }

        return RunSession(sdcardURL: sdcard, programURL: programURL)
    }

    /// Command-line arguments for the simulator.
    public static func simulatorArguments(for session: RunSession) -> [String] {
        let root = session.sdcardURL.standardizedFileURL.path
        let program = session.programURL.standardizedFileURL.path
        let prefix = root.hasSuffix("/") ? root : root + "/"
        let boot = program.hasPrefix(prefix) ? String(program.dropFirst(prefix.count)) : session.programURL.lastPathComponent
        return [
            "--sdcard", session.sdcardURL.path,
            "--boot", boot,
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

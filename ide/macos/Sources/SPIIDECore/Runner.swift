import Foundation

/// Where a run puts the simulator's virtual SD card and what it boots.
public struct RunSession: Sendable, Equatable {
    public let sdcardURL: URL
    /// Card-relative path of the program the simulator boots.
    public let bootPath: String
    /// True when the OS boots and the program is installed for the shell
    /// to run (applications and utilities).
    public let underOS: Bool

    public init(sdcardURL: URL, bootPath: String, underOS: Bool) {
        self.sdcardURL = sdcardURL
        self.bootPath = bootPath
        self.underOS = underOS
    }
}

public enum RunError: Error, LocalizedError, Equatable {
    case noCardImage
    case cardImageHasNoOS(String)

    public var errorDescription: String? {
        switch self {
        case .noCardImage:
            "Running an application or utility needs the OS: set the SD card image folder in Settings (it must contain core/boot.prg or core/boot.lua)."
        case .cardImageHasNoOS(let path):
            "The SD card image at \(path) has no core/boot.prg or core/boot.lua."
        }
    }
}

/// Prepares a run folder (`build/run/sdcard`) for the simulator.
///
/// - Raw programs and games boot directly: the product is copied into
///   the run card (`data/` or `games/`) and the simulator boots it with
///   no OS underneath.
/// - Applications and utilities run under the OS: `core/` is copied
///   from the user's SD card image, the bundle is installed where the
///   shell will find it, and the simulator boots `core/boot`.
public enum Runner {
    public static func runDirectory(for project: Project) -> URL {
        project.buildDirectoryURL.appendingPathComponent("run")
    }

    public static func prepare(
        project: Project, build: BuildProduct, cardImage: URL? = nil
    ) throws -> RunSession {
        let fm = FileManager.default
        let sdcard = runDirectory(for: project).appendingPathComponent("sdcard", isDirectory: true)
        for folder in ["core", "apps", "utils", "games", "data"] {
            try fm.createDirectory(
                at: sdcard.appendingPathComponent(folder, isDirectory: true),
                withIntermediateDirectories: true)
        }

        if project.manifest.kind.runsUnderOS {
            guard let cardImage else { throw RunError.noCardImage }
            let imageCore = cardImage.appendingPathComponent("core", isDirectory: true)
            let hasBoot = ["boot.prg", "boot.lua"].contains {
                fm.fileExists(atPath: imageCore.appendingPathComponent($0).path)
            }
            guard hasBoot else { throw RunError.cardImageHasNoOS(cardImage.path) }
            // The OS from the image, fresh each run.
            let core = sdcard.appendingPathComponent("core", isDirectory: true)
            try? fm.removeItem(at: core)
            try fm.copyItem(at: imageCore, to: core)
            try ProjectInstaller.install(project, into: sdcard)
            let boot = fm.fileExists(atPath: core.appendingPathComponent("boot.prg").path)
                ? "core/boot.prg" : "core/boot.lua"
            return RunSession(sdcardURL: sdcard, bootPath: boot, underOS: true)
        }

        // Direct boot: the product itself, installed into the run card.
        // A missing .prg (no simulator to compile with) falls back to the
        // generated source.
        if project.bundleURL != nil {
            let written = try ProjectInstaller.install(project, into: sdcard)
            let bundle = written[0]
            return RunSession(
                sdcardURL: sdcard,
                bootPath: relativePath(bundle.appendingPathComponent("app.prg"), in: sdcard),
                underOS: false)
        }
        let directory = sdcard.appendingPathComponent(project.installDirectory, isDirectory: true)
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let sourceURL = directory.appendingPathComponent(project.programFileName)
        try Data(build.lua.utf8).write(to: sourceURL, options: .atomic)
        var bootURL = sourceURL
        if fm.fileExists(atPath: project.prgProductURL.path) {
            let compiledURL = directory.appendingPathComponent(project.prgFileName)
            try? fm.removeItem(at: compiledURL)
            try fm.copyItem(at: project.prgProductURL, to: compiledURL)
            bootURL = compiledURL
        }
        return RunSession(sdcardURL: sdcard, bootPath: relativePath(bootURL, in: sdcard), underOS: false)
    }

    private static func relativePath(_ url: URL, in root: URL) -> String {
        let rootPath = root.standardizedFileURL.path
        let path = url.standardizedFileURL.path
        let prefix = rootPath.hasSuffix("/") ? rootPath : rootPath + "/"
        return path.hasPrefix(prefix) ? String(path.dropFirst(prefix.count)) : url.lastPathComponent
    }

    /// Command-line arguments for the simulator.
    public static func simulatorArguments(for session: RunSession) -> [String] {
        [
            "--sdcard", session.sdcardURL.path,
            "--boot", session.bootPath,
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

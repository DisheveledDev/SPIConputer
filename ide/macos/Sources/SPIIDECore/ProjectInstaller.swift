import Foundation

/// Copies a built product into an SD card image (a folder laid out like
/// the card: `core/`, `apps/`, `utils/`, `games/`, `data/`).
///
/// - raw programs install their `.prg` and `.lua` into the project's
///   install directory (`data/` unless the manifest says otherwise, e.g.
///   `core/` for the boot program and the shell);
/// - applications, utilities and games install their bundle into
///   `apps/`, `utils/` or `games/`.
public enum ProjectInstaller {
    public enum InstallError: Error, LocalizedError, Equatable {
        case notBuilt(String)
        case noCardImage

        public var errorDescription: String? {
            switch self {
            case .notBuilt(let what):
                "Nothing to install: \(what) has not been built (Build first)."
            case .noCardImage:
                "No SD card image folder is set (Settings > SD card image)."
            }
        }
    }

    /// Where the product lands inside `cardURL`.
    public static func destination(for project: Project, in cardURL: URL) -> URL {
        let directory = cardURL.appendingPathComponent(project.installDirectory, isDirectory: true)
        if let bundleName = project.bundleName {
            return directory.appendingPathComponent(bundleName, isDirectory: true)
        }
        return directory.appendingPathComponent(project.prgFileName)
    }

    /// Installs the current build. Returns the paths written.
    @discardableResult
    public static func install(_ project: Project, into cardURL: URL) throws -> [URL] {
        let fm = FileManager.default
        let directory = cardURL.appendingPathComponent(project.installDirectory, isDirectory: true)
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        // Every image gets the card's standard folders, so the OS finds
        // them even if only one thing was installed.
        for folder in ["core", "apps", "utils", "games", "data"] {
            try fm.createDirectory(
                at: cardURL.appendingPathComponent(folder, isDirectory: true),
                withIntermediateDirectories: true)
        }

        if let bundleURL = project.bundleURL, let bundleName = project.bundleName {
            guard fm.fileExists(atPath: bundleURL.appendingPathComponent("app.prg").path) else {
                throw InstallError.notBuilt(bundleName)
            }
            let destination = directory.appendingPathComponent(bundleName, isDirectory: true)
            try? fm.removeItem(at: destination)
            try fm.copyItem(at: bundleURL, to: destination)
            return [destination]
        }

        guard fm.fileExists(atPath: project.prgProductURL.path)
                || fm.fileExists(atPath: project.buildProductURL.path)
        else {
            throw InstallError.notBuilt(project.prgFileName)
        }
        var written: [URL] = []
        for source in [project.prgProductURL, project.buildProductURL]
        where fm.fileExists(atPath: source.path) {
            let destination = directory.appendingPathComponent(source.lastPathComponent)
            try? fm.removeItem(at: destination)
            try fm.copyItem(at: source, to: destination)
            written.append(destination)
        }
        return written
    }
}

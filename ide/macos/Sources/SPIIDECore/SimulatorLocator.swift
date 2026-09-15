import Foundation

/// Finds the simulator binary. Searches an explicit path, the environment,
/// the copy vendored in this package's resources, the app bundle, and
/// workspace folders containing a freshly built simulator.
public enum SimulatorLocator {
    public static let relativePath = "simulator/build/spicomputer_sim"
    public static let environmentKey = "SPICOMPUTER_SIMULATOR"

    /// `includeBundled` false skips the vendored copy (tests of the
    /// workspace search).
    public static func locate(
        startingAt starts: [URL],
        explicitPath: String? = nil,
        environment: [String: String] = ProcessInfo.processInfo.environment,
        includeBundled: Bool = true
    ) -> URL? {
        var candidates: [URL] = []
        if let explicitPath, !explicitPath.isEmpty {
            candidates.append(expanded(explicitPath))
        }
        if let fromEnvironment = environment[environmentKey], !fromEnvironment.isEmpty {
            candidates.append(expanded(fromEnvironment))
        }
        if includeBundled, let bundled = BundledResources.simulatorURL {
            candidates.append(bundled)
        }
        if let resourceURL = Bundle.main.resourceURL {
            candidates.append(resourceURL.appendingPathComponent("simulator/spicomputer_sim"))
        }
        for start in starts {
            candidates.append(contentsOf: search(from: start))
        }
        return candidates.first { isExecutable($0) }
    }

    /// Candidate locations from `start` upward (nearest first).
    public static func search(from start: URL) -> [URL] {
        var results: [URL] = []
        var directory = start.standardizedFileURL
        if !directory.hasDirectoryPath {
            directory.deleteLastPathComponent()
        }
        for _ in 0..<10 {
            results.append(directory.appendingPathComponent(relativePath))
            if directory.lastPathComponent == "simulator" {
                results.append(directory.appendingPathComponent("build/spicomputer_sim"))
            }
            let parent = directory.deletingLastPathComponent()
            if parent.path == directory.path {
                break
            }
            directory = parent
        }
        return results
    }

    private static func expanded(_ path: String) -> URL {
        URL(fileURLWithPath: (path as NSString).expandingTildeInPath)
    }

    private static func isExecutable(_ url: URL) -> Bool {
        FileManager.default.isExecutableFile(atPath: url.path)
    }
}

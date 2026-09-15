import Foundation

import SPIIDECore

/// Headless counterpart of the IDE's Build (and Install) actions:
///
///     swift run spibuild <project-folder>
///     swift run spibuild --install <sdcard-image> <project-folder>
///
/// Loads the project's manifest, compiles its components into the single
/// Lua program under the project's build/ folder (plus the .prg and the
/// kind's bundle when the simulator is available), and with --install
/// copies the product into the card image (core/, apps/, utils/, games/
/// or data/ by kind).

var arguments = Array(CommandLine.arguments.dropFirst())
var installTarget: URL?
if let index = arguments.firstIndex(of: "--install"), index + 1 < arguments.count {
    installTarget = URL(fileURLWithPath: arguments[index + 1], isDirectory: true).standardizedFileURL
    arguments.removeSubrange(index...(index + 1))
}
guard let projectPath = arguments.first else {
    FileHandle.standardError.write(Data("usage: spibuild [--install <sdcard-image>] <project-folder>\n".utf8))
    exit(2)
}

let root = URL(fileURLWithPath: projectPath, isDirectory: true).standardizedFileURL
do {
    let project = try ProjectStore.load(from: root)
    let product = try ProjectBuilder.build(project)
    try? FileManager.default.removeItem(at: project.prgProductURL)
    if let bundleURL = project.bundleURL {
        try? FileManager.default.removeItem(at: bundleURL)
    }

    let executable = URL(fileURLWithPath: CommandLine.arguments[0])
    let cwd = URL(fileURLWithPath: FileManager.default.currentDirectoryPath, isDirectory: true)
    let simulator = SimulatorLocator.locate(
        startingAt: Runner.defaultSimulatorSearchStarts(executable: executable, cwd: cwd))
    var compiled = false
    if let simulator {
        let outcome = PrgCompiler.compile(
            source: product.outputURL,
            output: project.prgProductURL,
            simulator: simulator)
        if outcome.outputURL != nil {
            try ProjectBuilder.writeAppBundle(project, compiledURL: project.prgProductURL)
            compiled = true
        } else if let error = outcome.error {
            FileHandle.standardError.write(Data("spibuild: .prg compilation failed: \(error)\n".utf8))
            exit(1)
        } else if let reason = outcome.unavailableReason {
            FileHandle.standardError.write(Data("spibuild: .prg compilation unavailable: \(reason)\n".utf8))
        }
    } else {
        FileHandle.standardError.write(Data("spibuild: simulator not found, skipped .prg output\n".utf8))
    }

    let productPath = (project.bundleURL ?? project.prgProductURL).standardizedFileURL.path
    let output = compiled ? " + \(productPath)" : ""
    print("built \(product.outputURL.standardizedFileURL.path)\(output) "
        + "(\(project.manifest.kind.rawValue), \(product.componentCount) components, \(product.lua.utf8.count) bytes)")

    if let installTarget {
        let written = try ProjectInstaller.install(project, into: installTarget)
        for url in written {
            print("installed \(url.standardizedFileURL.path)")
        }
    }
} catch {
    FileHandle.standardError.write(
        Data("spibuild: \(error.localizedDescription)\n".utf8))
    exit(1)
}

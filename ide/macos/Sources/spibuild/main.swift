import Foundation

import SPIIDECore

/// Headless counterpart of the IDE's Build action:
///
///     swift run spibuild <project-folder>
///
/// Loads the project's manifest, compiles its components into the single
/// Lua program and writes it to the project's output directory, so a
/// project can be built from a terminal or CI as well as from the IDE.

let arguments = CommandLine.arguments.dropFirst()
guard let projectPath = arguments.first else {
    FileHandle.standardError.write(Data("usage: spibuild <project-folder>\n".utf8))
    exit(2)
}

let root = URL(fileURLWithPath: projectPath, isDirectory: true).standardizedFileURL
do {
    let project = try ProjectStore.load(from: root)
    let product = try ProjectBuilder.build(project)
    try? FileManager.default.removeItem(at: project.prgProductURL)

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
            compiled = true
        } else if let error = outcome.error {
            FileHandle.standardError.write(Data("spibuild: .prg compilation failed: \(error)\n".utf8))
        } else if let reason = outcome.unavailableReason {
            FileHandle.standardError.write(Data("spibuild: .prg compilation unavailable: \(reason)\n".utf8))
        }
    } else {
        FileHandle.standardError.write(Data("spibuild: simulator not found, skipped .prg output\n".utf8))
    }

    let output = compiled ? " + \(project.prgProductURL.standardizedFileURL.path)" : ""
    print("built \(product.outputURL.standardizedFileURL.path)\(output) "
        + "(\(product.componentCount) components, \(product.lua.utf8.count) bytes)")
} catch {
    FileHandle.standardError.write(
        Data("spibuild: \(error.localizedDescription)\n".utf8))
    exit(1)
}

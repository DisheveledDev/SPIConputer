import Foundation
import Testing

@testable import SPIIDECore

@Suite("Simulator locator")
struct SimulatorLocatorTests {
    /// Builds a fake workspace: <root>/system, <root>/simulator/build/spicomputer_sim.
    private func makeWorkspace(withSimulator: Bool) throws -> URL {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("spiide-ws-\(UUID().uuidString)")
        let fm = FileManager.default
        try fm.createDirectory(at: root.appendingPathComponent("system"), withIntermediateDirectories: true)
        let sim = root.appendingPathComponent(SimulatorLocator.relativePath)
        try fm.createDirectory(at: sim.deletingLastPathComponent(), withIntermediateDirectories: true)
        if withSimulator {
            fm.createFile(atPath: sim.path, contents: Data("#!/bin/sh\n".utf8))
            try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: sim.path)
        }
        return root
    }

    @Test func vendoredSimulatorAndCardImageShipInTheBundle() throws {
        let simulator = try #require(BundledResources.simulatorURL)
        #expect(FileManager.default.isExecutableFile(atPath: simulator.path))
        #expect(FileManager.default.fileExists(
            atPath: simulator.deletingLastPathComponent().appendingPathComponent("libSDL2-2.0.0.dylib").path),
            "the SDL library sits beside the simulator")
        // It runs from where it is (the library is found via @loader_path).
        let process = Process()
        process.executableURL = simulator
        process.arguments = ["--help"]
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        try process.run()
        process.waitUntilExit()
        let output = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        #expect(output.contains("--boot"), "simulator --help output: \(output.prefix(200))")

        let card = try #require(BundledResources.cardImageURL)
        for file in ["core/boot.prg", "core/os.prg"] {
            #expect(FileManager.default.fileExists(atPath: card.appendingPathComponent(file).path), "\(file) present")
        }
        // With no explicit path and no workspace, the locator lands on it.
        let found = SimulatorLocator.locate(startingAt: [], explicitPath: nil, environment: [:])
        #expect(found == simulator)
    }

    @Test func findsSimulatorWalkingUpFromTheAppBinary() throws {
        let root = try makeWorkspace(withSimulator: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let executable = root.appendingPathComponent("ide/macos/.build/debug/SPIIDE")
        try FileManager.default.createDirectory(
            at: executable.deletingLastPathComponent(), withIntermediateDirectories: true)

        let found = SimulatorLocator.locate(
            startingAt: [executable], explicitPath: nil, environment: [:], includeBundled: false)
        #expect(found?.path == root.appendingPathComponent(SimulatorLocator.relativePath).path)
    }

    @Test func returnsNilWhenMissing() throws {
        let root = try makeWorkspace(withSimulator: false)
        defer { try? FileManager.default.removeItem(at: root) }

        let executable = root.appendingPathComponent("ide/macos/.build/debug/SPIIDE")
        let found = SimulatorLocator.locate(
            startingAt: [executable], explicitPath: nil, environment: [:], includeBundled: false)
        #expect(found == nil)
    }

    @Test func explicitPathWins() throws {
        let root = try makeWorkspace(withSimulator: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let sim = root.appendingPathComponent(SimulatorLocator.relativePath)

        let found = SimulatorLocator.locate(
            startingAt: [], explicitPath: sim.path, environment: [:])
        #expect(found?.path == sim.path)
    }

    @Test func environmentPathWorks() throws {
        let root = try makeWorkspace(withSimulator: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let sim = root.appendingPathComponent(SimulatorLocator.relativePath)

        let found = SimulatorLocator.locate(
            startingAt: [], explicitPath: nil,
            environment: [SimulatorLocator.environmentKey: sim.path])
        #expect(found?.path == sim.path)
    }
}

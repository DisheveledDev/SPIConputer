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

    @Test func findsSimulatorWalkingUpFromTheAppBinary() throws {
        let root = try makeWorkspace(withSimulator: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let executable = root.appendingPathComponent("ide/macos/.build/debug/SPIIDE")
        try FileManager.default.createDirectory(
            at: executable.deletingLastPathComponent(), withIntermediateDirectories: true)

        let found = SimulatorLocator.locate(
            startingAt: [executable], explicitPath: nil, environment: [:])
        #expect(found?.path == root.appendingPathComponent(SimulatorLocator.relativePath).path)
    }

    @Test func returnsNilWhenMissing() throws {
        let root = try makeWorkspace(withSimulator: false)
        defer { try? FileManager.default.removeItem(at: root) }

        let executable = root.appendingPathComponent("ide/macos/.build/debug/SPIIDE")
        let found = SimulatorLocator.locate(
            startingAt: [executable], explicitPath: nil, environment: [:])
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

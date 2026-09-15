// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "SPIIDE",
    platforms: [
        .macOS(.v14)
    ],
    targets: [
        .target(
            name: "SPIIDECore",
            resources: [
                // Frameworks injected into programs (read-only Lua).
                .copy("Resources/sdk"),
                // The OS simulator (with its SDL library beside it): the
                // .prg compiler and the Run target, vendored so this
                // package builds and runs without the OS workspace.
                .copy("Resources/simulator"),
                // A minimal card image: core/boot and core/os, so an
                // application can run under the OS out of the box.
                .copy("Resources/sdcard"),
                // The help documents (Lua, OS API, frameworks).
                .copy("Resources/help"),
            ]
        ),
        .executableTarget(
            name: "SPIIDE",
            dependencies: ["SPIIDECore"],
            resources: [.process("Resources")]
        ),
        .executableTarget(
            name: "spibuild",
            dependencies: ["SPIIDECore"]
        ),
        .testTarget(
            name: "SPIIDECoreTests",
            dependencies: ["SPIIDECore"]
        ),
        .testTarget(
            name: "SPIIDETests",
            dependencies: ["SPIIDE"]
        ),
    ]
)

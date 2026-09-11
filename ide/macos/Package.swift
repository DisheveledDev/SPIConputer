// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "SPIIDE",
    platforms: [
        .macOS(.v14)
    ],
    targets: [
        .target(name: "SPIIDECore"),
        .executableTarget(
            name: "SPIIDE",
            dependencies: ["SPIIDECore"],
            resources: [.process("Resources")]
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

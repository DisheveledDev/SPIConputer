// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "SPITerminal",
    platforms: [
        .macOS(.v13)
    ],
    targets: [
        // C: serial-mirror protocol parser + vendored font8x8 ROM font.
        .target(name: "TerminalC"),
        .executableTarget(
            name: "SPITerminal",
            dependencies: ["TerminalC"]
        ),
    ]
)

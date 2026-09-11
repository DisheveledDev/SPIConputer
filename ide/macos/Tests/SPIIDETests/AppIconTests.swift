import AppKit
import Testing

@testable import SPIIDE

/// The app is launched as a bare executable (`swift run`), so the Dock
/// icon comes from a packaged resource rather than an app bundle.
@Suite("App icon")
@MainActor
struct AppIconTests {
    @Test func appIconResourceLoads() throws {
        let url = try #require(
            Bundle.module.url(forResource: "AppIcon", withExtension: "png"),
            "AppIcon.png missing from the resource bundle")
        let image = try #require(NSImage(contentsOf: url))
        #expect(image.size.width >= 128)
        #expect(image.size.height == image.size.width)
    }
}

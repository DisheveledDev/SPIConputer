import Foundation

/// What ships inside this package's resource bundle (see Package.swift):
/// the OS simulator and a minimal card image. Both come from the OS
/// workspace and are refreshed with `scripts/update-vendor.sh`.
public enum BundledResources {
    /// The vendored simulator binary, or nil when the bundle lacks it.
    public static var simulatorURL: URL? {
        guard let url = Bundle.module.url(
            forResource: "spicomputer_sim", withExtension: nil, subdirectory: "simulator"),
              FileManager.default.isExecutableFile(atPath: url.path)
        else { return nil }
        return url
    }

    /// The vendored card image (`core/boot.prg`, `core/os.prg`), or nil.
    /// Read-only: Run copies its `core/` out; Install needs a folder of
    /// the user's own.
    public static var cardImageURL: URL? {
        guard let url = Bundle.module.url(forResource: "sdcard", withExtension: nil),
              FileManager.default.fileExists(
                atPath: url.appendingPathComponent("core/boot.prg").path)
        else { return nil }
        return url
    }
}

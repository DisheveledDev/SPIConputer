import Foundation

/// A tile/palette asset, edited in the IDE and compiled at build time to
/// `ScreenPaletteSet` / `ScreenDefineTile` calls.
public struct TilesAsset: Codable, Sendable, Equatable {
    /// Optional palette: entry `i` (0-based) becomes palette index `i`.
    public var palette: [RGB]?
    /// Tiles keyed by their char-map index (0-255).
    public var tiles: [TileDef]

    public init(palette: [RGB]? = nil, tiles: [TileDef] = []) {
        self.palette = palette
        self.tiles = tiles
    }

    enum CodingKeys: String, CodingKey {
        case palette, tiles
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        palette = try c.decodeIfPresent([RGB].self, forKey: .palette)
        tiles = try c.decodeIfPresent([TileDef].self, forKey: .tiles) ?? []
    }
}

/// One 8x8 tile. `rows` holds 8 bit patterns (bit 0 = leftmost pixel,
/// matching the ROM font and `ScreenDefineTile`).
public struct TileDef: Codable, Sendable, Equatable {
    public var index: Int
    public var rows: [Int]

    public init(index: Int, rows: [Int] = Array(repeating: 0, count: 8)) {
        self.index = index
        self.rows = rows
    }

    enum CodingKeys: String, CodingKey {
        case index, rows
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        index = try c.decodeIfPresent(Int.self, forKey: .index) ?? 32
        rows = try c.decodeIfPresent([Int].self, forKey: .rows)
            ?? Array(repeating: 0, count: 8)
    }

    public func normalized() -> TileDef {
        var r = Array(rows.prefix(8))
        while r.count < 8 {
            r.append(0)
        }
        return TileDef(index: min(max(index, 0), 255), rows: r.map { min(max($0, 0), 255) })
    }
}

/// An 8-bit-per-channel colour, matching `ScreenPalette(r, g, b)`.
public struct RGB: Codable, Sendable, Equatable, Hashable {
    public var r: Int
    public var g: Int
    public var b: Int

    public init(r: Int, g: Int, b: Int) {
        self.r = r
        self.g = g
        self.b = b
    }

    enum CodingKeys: String, CodingKey {
        case r, g, b
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        r = try c.decodeIfPresent(Int.self, forKey: .r) ?? 0
        g = try c.decodeIfPresent(Int.self, forKey: .g) ?? 0
        b = try c.decodeIfPresent(Int.self, forKey: .b) ?? 0
    }
}

extension TilesAsset {
    /// The OS default C64-ish palette, used when a tiles component has no
    /// explicit palette and the editor needs something to show.
    public static let defaultPalette: [RGB] = [
        RGB(r: 0x00, g: 0x00, b: 0x00),
        RGB(r: 0xff, g: 0xff, b: 0xff),
        RGB(r: 0x88, g: 0x00, b: 0x00),
        RGB(r: 0xaa, g: 0xff, b: 0xee),
        RGB(r: 0xcc, g: 0x44, b: 0xcc),
        RGB(r: 0x00, g: 0xcc, b: 0x55),
        RGB(r: 0x00, g: 0x00, b: 0xaa),
        RGB(r: 0xee, g: 0xee, b: 0x77),
        RGB(r: 0xdd, g: 0x88, b: 0x55),
        RGB(r: 0x66, g: 0x44, b: 0x00),
        RGB(r: 0xff, g: 0x77, b: 0x77),
        RGB(r: 0x33, g: 0x33, b: 0x33),
        RGB(r: 0x77, g: 0x77, b: 0x77),
        RGB(r: 0xaa, g: 0xff, b: 0x66),
        RGB(r: 0x00, g: 0x88, b: 0xff),
        RGB(r: 0xbb, g: 0xbb, b: 0xbb),
    ]
}

import CoreGraphics
import Foundation
import TerminalC

/// Display state driven by the serial-mirror protocol. Records arrive on
/// the main thread via ProtocolSession, so no locking is needed.
@MainActor
final class TerminalModel: ObservableObject {
    @Published private(set) var frameVersion: UInt64 = 0
    @Published private(set) var connected = false

    private(set) var width = 40
    private(set) var height = 30
    private(set) var charMap = [UInt8](repeating: 32, count: 40 * 30)

    // Per-cell tile overrides; the ROM font (font8x8) is used until a
    // `tile=` record defines a custom glyph.
    private var tileOverrides = [[UInt8]](repeating: [UInt8](repeating: 0, count: 8),
                                          count: 256)
    private var tileDefined = [Bool](repeating: false, count: 256)

    private(set) var fg = (r: UInt8(238), g: UInt8(238), b: UInt8(119)) // yellow
    private(set) var bg = (r: UInt8(0), g: UInt8(0), b: UInt8(170))     // darkblue

    func setConnected(_ value: Bool) {
        connected = value
    }

    func apply(_ rec: proto_record_t) {
        var mutableRec = rec
        switch rec.type {
        case PROTO_REC_RESOLUTION:
            resize(Int(rec.u.res.w), Int(rec.u.res.h))
        case PROTO_REC_FOREGROUND:
            fg = (rec.u.colour.r, rec.u.colour.g, rec.u.colour.b)
            frameVersion &+= 1
        case PROTO_REC_BACKGROUND:
            bg = (rec.u.colour.r, rec.u.colour.g, rec.u.colour.b)
            frameVersion &+= 1
        case PROTO_REC_TILE:
            let index = Int(rec.u.tile.index) & 0xff
            var rows = [UInt8](repeating: 0, count: 8)
            if let rowsPtr = proto_tile_rows(&mutableRec) {
                for row in 0..<8 { rows[row] = rowsPtr[row] }
            }
            tileOverrides[index] = rows
            tileDefined[index] = true
            frameVersion &+= 1
        case PROTO_REC_DATA:
            let count = Int(proto_frame_count(&mutableRec))
            let cells = width * height
            if count <= cells, let valsPtr = proto_frame_vals(&mutableRec) {
                charMap.withUnsafeMutableBufferPointer { dst in
                    for i in 0..<count { dst[i] = valsPtr[i] }
                }
                frameVersion &+= 1
            }
        default:
            break // PROTO_REC_NONE / PROTO_REC_UNKNOWN: ignore
        }
    }

    private func resize(_ w: Int, _ h: Int) {
        guard w > 0, h > 0, w * h <= Int(PROTO_MAX_FRAME_CELLS) else { return }
        guard w != width || h != height else { return }
        width = w
        height = h
        charMap = [UInt8](repeating: 32, count: w * h)
        frameVersion &+= 1
    }

    func glyphRows(for ch: UInt8) -> [UInt8] {
        if tileDefined[Int(ch) & 0xff] {
            return tileOverrides[Int(ch) & 0xff]
        }
        var rows = [UInt8](repeating: 0, count: 8)
        guard let font = spiterm_font8x8(ch) else { return rows }
        for row in 0..<8 { rows[row] = UInt8(bitPattern: font[row]) }
        return rows
    }

    /// Rasterises the frame into an RGBA image at `scale` pixels per tile
    /// pixel (2 -> crisp 640x480 for 40x30).
    func renderImage(scale: Int) -> CGImage? {
        let scale = max(1, scale)
        let w = width * 8 * scale
        let h = height * 8 * scale
        var pixels = [UInt8](repeating: 0, count: w * h * 4)

        pixels.withUnsafeMutableBufferPointer { buf in
            for cy in 0..<height {
                for cx in 0..<width {
                    let glyph = glyphRows(for: charMap[cy * width + cx])
                    for row in 0..<8 {
                        let bits = glyph[row]
                        for col in 0..<8 {
                            let on = (bits & (0x80 >> col)) != 0
                            let (r, g, b) = on ? (fg.r, fg.g, fg.b)
                                              : (bg.r, bg.g, bg.b)
                            for sy in 0..<scale {
                                let y = (cy * 8 + row) * scale + sy
                                for sx in 0..<scale {
                                    let x = (cx * 8 + col) * scale + sx
                                    let i = (y * w + x) * 4
                                    buf[i] = r
                                    buf[i + 1] = g
                                    buf[i + 2] = b
                                    buf[i + 3] = 255
                                }
                            }
                        }
                    }
                }
            }
        }

        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(
                  data: &pixels, width: w, height: h,
                  bitsPerComponent: 8, bytesPerRow: w * 4, space: space,
                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
        else { return nil }
        return ctx.makeImage()
    }
}

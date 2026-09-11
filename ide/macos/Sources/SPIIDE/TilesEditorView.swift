import AppKit
import SwiftUI

import SPIIDECore

/// Editor for a `tiles` component: tile list, 8x8 pixel grid and palette.
struct TilesEditorView: View {
    @Environment(AppModel.self) private var model
    @State private var selectedTile = 0

    var body: some View {
        @Bindable var model = model

        HSplitView {
            tileList
                .frame(minWidth: 150, maxWidth: 200)
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    tileEditor(tiles: $model.tilesAsset.tiles)
                    paletteEditor(palette: paletteBinding)
                }
                .padding()
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .onChange(of: model.tilesAsset) {
            model.scheduleSave()
        }
    }

    // MARK: Tile list

    private var tileList: some View {
        VStack(spacing: 0) {
            List(selection: $selectedTile) {
                ForEach(model.tilesAsset.tiles.indices, id: \.self) { index in
                    Text("Tile \(model.tilesAsset.tiles[index].index)")
                        .tag(index)
                }
            }
            Divider()
            HStack {
                Button("Add Tile", systemImage: "plus") { addTile() }
                    .labelStyle(.iconOnly)
                Button("Remove Tile", systemImage: "minus") { removeTile() }
                    .labelStyle(.iconOnly)
                    .disabled(model.tilesAsset.tiles.isEmpty)
                Spacer()
            }
            .buttonStyle(.borderless)
            .padding(6)
        }
    }

    private func addTile() {
        let used = Set(model.tilesAsset.tiles.map(\.index))
        let next = (32...255).first { !used.contains($0) } ?? 128
        model.tilesAsset.tiles.append(TileDef(index: next))
        selectedTile = model.tilesAsset.tiles.count - 1
    }

    private func removeTile() {
        guard model.tilesAsset.tiles.indices.contains(selectedTile) else { return }
        model.tilesAsset.tiles.remove(at: selectedTile)
        selectedTile = max(0, min(selectedTile, model.tilesAsset.tiles.count - 1))
    }

    // MARK: Grid

    @ViewBuilder
    private func tileEditor(tiles: Binding<[TileDef]>) -> some View {
        GroupBox("Tile") {
            if tiles.wrappedValue.indices.contains(selectedTile) {
                VStack(alignment: .leading, spacing: 12) {
                    HStack {
                        Text("Char index")
                        TextField("Index", value: tiles[selectedTile].index, format: .number)
                            .frame(width: 64)
                        Text("(0-255, ASCII-aligned: 65 = 'A')")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    TileGridView(rows: tiles[selectedTile].rows)
                    Text("Click pixels to toggle. Bit 7 is the leftmost pixel.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .padding(8)
            } else {
                Text("Add a tile to start drawing.")
                    .foregroundStyle(.secondary)
                    .padding(8)
            }
        }
    }

    // MARK: Palette

    private var paletteBinding: Binding<[RGB]> {
        Binding(
            get: { model.tilesAsset.palette ?? TilesAsset.defaultPalette },
            set: { model.tilesAsset.palette = $0 })
    }

    private func paletteEditor(palette: Binding<[RGB]>) -> some View {
        GroupBox("Palette") {
            VStack(alignment: .leading, spacing: 8) {
                Text("Entry 0 is the background; attribute colours 0-6 use entries 1-7.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                ForEach(palette.wrappedValue.indices, id: \.self) { index in
                    HStack {
                        Text("\(index)")
                            .font(.system(.caption, design: .monospaced))
                            .frame(width: 24, alignment: .trailing)
                        ColorPicker(
                            "Colour \(index)",
                            selection: colorBinding(palette: palette, index: index),
                            supportsOpacity: false)
                            .labelsHidden()
                        Spacer()
                        Button("Remove", systemImage: "minus") {
                            palette.wrappedValue.remove(at: index)
                        }
                        .labelStyle(.iconOnly)
                        .buttonStyle(.borderless)
                    }
                }
                Button("Add Colour", systemImage: "plus") {
                    palette.wrappedValue.append(RGB(r: 255, g: 255, b: 255))
                }
            }
            .padding(8)
        }
    }

    private func colorBinding(palette: Binding<[RGB]>, index: Int) -> Binding<Color> {
        Binding(
            get: {
                guard palette.wrappedValue.indices.contains(index) else { return .white }
                return Color(rgb: palette.wrappedValue[index])
            },
            set: { newValue in
                guard palette.wrappedValue.indices.contains(index) else { return }
                palette.wrappedValue[index] = newValue.rgbComponents
            })
    }
}

/// An 8x8 pixel toggle grid.
struct TileGridView: View {
    @Binding var rows: [Int]

    var body: some View {
        VStack(spacing: 2) {
            ForEach(0..<8, id: \.self) { y in
                HStack(spacing: 2) {
                    ForEach(0..<8, id: \.self) { x in
                        Button {
                            toggle(x: x, y: y)
                        } label: {
                            Rectangle()
                                .fill(isOn(x: x, y: y)
                                    ? Color.primary
                                    : Color.secondary.opacity(0.12))
                                .frame(width: 24, height: 24)
                                .overlay(Rectangle().stroke(.quaternary, lineWidth: 1))
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(
                            "Pixel \(x + 1), \(y + 1) \(isOn(x: x, y: y) ? "on" : "off")")
                    }
                }
            }
        }
    }

    private func isOn(x: Int, y: Int) -> Bool {
        guard rows.indices.contains(y) else { return false }
        return (rows[y] >> (7 - x)) & 1 == 1
    }

    private func toggle(x: Int, y: Int) {
        while rows.count < 8 {
            rows.append(0)
        }
        rows[y] ^= 1 << (7 - x)
    }
}

extension Color {
    init(rgb: RGB) {
        self.init(
            .sRGB,
            red: Double(rgb.r) / 255.0,
            green: Double(rgb.g) / 255.0,
            blue: Double(rgb.b) / 255.0)
    }

    var rgbComponents: RGB {
        let color = NSColor(self).usingColorSpace(.sRGB) ?? .white
        return RGB(
            r: Int((color.redComponent * 255).rounded()),
            g: Int((color.greenComponent * 255).rounded()),
            b: Int((color.blueComponent * 255).rounded()))
    }
}

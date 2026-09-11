import SwiftUI

/// Draws the current frame, letterboxed, pixel-crisp.
struct DisplayView: View {
    @ObservedObject var model: TerminalModel

    var body: some View {
        Canvas { context, size in
            guard size.width > 0, size.height > 0,
                  let image = model.renderImage(scale: 2)
            else { return }

            let imgW = CGFloat(image.width)
            let imgH = CGFloat(image.height)
            let scale = min(size.width / imgW, size.height / imgH)
            let drawW = imgW * scale
            let drawH = imgH * scale
            let rect = CGRect(x: (size.width - drawW) / 2,
                              y: (size.height - drawH) / 2,
                              width: drawW, height: drawH)
            context.draw(Image(decorative: image, scale: 1),
                         in: rect)
        }
        .background(Color.black)
    }
}

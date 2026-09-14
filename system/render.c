/* render.c — see render.h */
#include "render.h"

#include "font8x8_basic.h"

void render_line(const video_state_t *v, int ly, uint8_t *out) {
    int row = ly / 8;
    int sub = ly % 8;

    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int lpx = x / 2; /* 2x horizontal scaling */
        int col = lpx / 8;
        int bit = lpx % 8;
        uint32_t colour;

        if (v->mode == VIDEO_MODE_PIXEL) {
            uint8_t ci =
                v->framebuf ? v->framebuf[ly * VIDEO_FB_COLS + lpx] : 0;
            colour = v->palette[ci];
        } else {
            uint8_t ch = v->char_map[0][row * VIDEO_COLS + col];
            uint8_t attr = v->attr_map[0][row * VIDEO_COLS + col];
            for (int layer = VIDEO_LAYERS - 1; layer > 0; layer--) {
                if (!v->layer_active[layer]) {
                    continue;
                }
                uint8_t candidate = v->attr_map[layer][row * VIDEO_COLS + col];
                if ((candidate & VIDEO_ATTR_TRANSPARENT) == 0) {
                    ch = v->char_map[layer][row * VIDEO_COLS + col];
                    attr = candidate;
                    break;
                }
            }
            uint8_t bits;
            if (v->tile_defined[ch]) {
                bits = v->tiles[ch][sub];
            } else {
                bits = font8x8_basic[ch & 0x7f][sub];
            }
            int on = (bits >> bit) & 1;

            uint8_t fg, bg;
            if (v->mode == VIDEO_MODE_TEXT40C) {
                fg = (uint8_t)((attr & 0x07) + 1);
                bg = 0;
            } else {
                fg = 1;
                bg = 0;
            }
            if (attr & 0x80) {
                uint8_t t = fg;
                fg = bg;
                bg = t;
            }
            colour = v->palette[on ? fg : bg];
        }

        out[x * 3 + 0] = (uint8_t)(colour >> 16);
        out[x * 3 + 1] = (uint8_t)(colour >> 8);
        out[x * 3 + 2] = (uint8_t)colour;
    }
}

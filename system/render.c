/* render.c — see render.h */
#include "render.h"

#include <stdbool.h>

#include "font8x8_rom.h"

void render_line(const video_state_t *v, int ly, uint8_t *out) {
    int row = ly / 8;
    int sub = ly % 8;
    bool x2 = video_mode_2x(v->mode);
    int cols = video_mode_cols(v->mode);
    bool colour_mode = video_mode_colour(v->mode);

    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int lpx = x2 ? x / 2 : x; /* 2x modes double every pixel */
        int col = lpx / 8;
        int bit = lpx % 8;
        uint32_t colour;

        if (v->mode == VIDEO_MODE_PIXEL) {
            uint8_t ci =
                v->framebuf ? v->framebuf[ly * VIDEO_FB_COLS + lpx] : 0;
            colour = v->palette[ci];
        } else if (v->mode == VIDEO_MODE_PIXEL_LO) {
            /* 4x4 pixels: quarter the column, halve the logical row. */
            uint8_t ci = v->framebuf
                ? v->framebuf[(ly / 2) * VIDEO_FB_LO_COLS + x / 4] : 0;
            colour = v->palette[ci];
        } else {
            int idx = row * cols + col;
            uint8_t ch = v->base_char[idx];
            uint8_t attr = v->base_attr[idx];
            uint8_t oattr = v->overlay_attr[idx];
            if ((oattr & VIDEO_ATTR_TRANSPARENT) == 0) {
                ch = v->overlay_char[idx];
                attr = oattr;
            }
            uint8_t bits;
            if (v->tile_defined[ch]) {
                bits = v->tiles[ch][sub];
            } else {
                bits = font8x8_rom[ch][sub];
            }
            int on = (bits >> bit) & 1;

            uint8_t fg, bg;
            if (colour_mode) {
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

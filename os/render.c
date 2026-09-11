/* render.c — see render.h */
#include "render.h"

#include "protocol.h" /* spiterm_font8x8: the ASCII-aligned ROM font */

void render_line(const video_state_t *v, int y, uint8_t *out) {
    int mode = v->mode;
    int cols, scale;

    if (mode == VIDEO_MODE_PIXEL) {
        cols = VIDEO_FB_COLS;
        scale = 2;
    } else if (mode == VIDEO_MODE_TEXT80 || mode == VIDEO_MODE_TEXT80C) {
        cols = 80;
        scale = 1;
    } else {
        cols = 40;
        scale = 2;
    }

    int ly = y / scale; /* logical pixel line (scale 1: y itself) */
    int row = ly / 8;   /* tile row within the char map */
    int sub = ly % 8;   /* subline within the tile */

    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int lpx = x / scale; /* logical pixel x */
        int col = lpx / 8;   /* tile column */
        int bit = lpx % 8;   /* pixel within the tile row */
        uint32_t colour;

        if (mode == VIDEO_MODE_PIXEL) {
            uint8_t ci = v->framebuf[ly * cols + lpx];
            colour = v->palette[ci];
        } else {
            uint8_t ch = v->char_map[row * cols + col];
            uint8_t attr = v->attr_map[row * cols + col];
            uint8_t bits;
            if (v->tile_defined[ch]) {
                bits = v->tiles[ch][sub];
            } else {
                bits = (uint8_t)spiterm_font8x8(ch)[sub];
            }
            int on = (bits >> bit) & 1; /* bit 0 is the leftmost pixel */

            uint8_t fg, bg;
            if (mode == VIDEO_MODE_TEXT40C || mode == VIDEO_MODE_TEXT80C) {
                fg = (uint8_t)((attr & 0x07) + 1); /* index c -> palette c+1 */
                bg = 0;                            /* black */
            } else {
                fg = 1; /* white */
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

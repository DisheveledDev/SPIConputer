/* render332.c — RGB332 fast scanline renderer; see render332.h */
#include "render332.h"

#include <stdbool.h>
#include <string.h>

#include "font8x8_basic.h"

/* Four output words containing 16 doubled pixels (one 8-pixel tile row
 * scaled 2x horizontally). */
static uint32_t s_expand2[4][256];
static uint32_t s_rgb332[256];
static bool s_palette_valid;

void render332_init(void) {
    memset(s_expand2, 0, sizeof(s_expand2));
    for (int bits = 0; bits < 256; bits++) {
        for (int word = 0; word < 4; word++) {
            uint32_t mask2 = 0;
            for (int pixel = 0; pixel < 4; pixel++) {
                int logical = (word * 4 + pixel) / 2;
                if (bits & (1 << logical)) {
                    mask2 |= 0xffu << (pixel * 8);
                }
            }
            s_expand2[word][bits] = mask2;
        }
    }
    for (int i = 0; i < 256; i++) {
        s_rgb332[i] = 0;
    }
    s_palette_valid = false;
}

void render332_invalidate_palette(void) {
    s_palette_valid = false;
}

/* Refresh the palette LUT from the state's RGB888 palette. Cached until
 * render332_invalidate_palette() (called after a queue drain that
 * changed the palette), not once per rendered line. */
static void update_palette(const video_state_t *v) {
    if (s_palette_valid) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        s_rgb332[i] = render332_rgb(v->palette[i]) * 0x01010101u;
    }
    s_palette_valid = true;
}

static void put_cell_2x(uint32_t *dst, uint8_t bits, uint32_t fg,
                        uint32_t bg) {
    for (int word = 0; word < 4; word++) {
        uint32_t mask = s_expand2[word][bits];
        dst[word] = (fg & mask) | (bg & ~mask);
    }
}

void render_line_332(const video_state_t *v, int ly, uint32_t *out) {
    update_palette(v);

    int row = ly / 8;
    int sub = ly % 8;

    if (v->mode == VIDEO_MODE_PIXEL) {
        /* Direct pixels: one logical line per output line pair; every
         * pixel is doubled horizontally. */
        const uint8_t *fb =
            v->framebuf ? v->framebuf + ly * VIDEO_FB_COLS : NULL;
        for (int x = 0; x < VIDEO_FB_COLS; x += 2) {
            uint8_t a = fb ? fb[x] : 0;
            uint8_t b = fb ? fb[x + 1] : 0;
            uint32_t px = s_rgb332[a] & 0xffffu;
            uint32_t px2 = s_rgb332[b] & 0xffffu;
            *out++ = px | (px2 << 16);
        }
        return;
    }

    for (int col = 0; col < VIDEO_COLS; col++) {
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
        uint8_t bits = v->tile_defined[ch] ? v->tiles[ch][sub]
                                           : (uint8_t)font8x8_basic[ch & 0x7f][sub];

        uint8_t fg_idx, bg_idx;
        if (v->mode == VIDEO_MODE_TEXT40C) {
            fg_idx = (uint8_t)((attr & 0x07) + 1);
            bg_idx = 0;
        } else {
            fg_idx = 1;
            bg_idx = 0;
        }
        if (attr & 0x80) {
            uint8_t t = fg_idx;
            fg_idx = bg_idx;
            bg_idx = t;
        }
        uint32_t fg = s_rgb332[fg_idx];
        uint32_t bg = s_rgb332[bg_idx];
        put_cell_2x(out, bits, fg, bg);
        out += 4;
    }
}

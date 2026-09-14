/* render332.c — RGB332 fast scanline renderer; see render332.h */
#include "render332.h"

#include <stdbool.h>
#include <string.h>

#include "font8x8_basic.h"

/* Masks for one font row. Each mask byte is either 0x00 or 0xff;
 * bit 0 of the font row is the leftmost pixel. */
static uint64_t s_expand1[256];
/* Four output words containing 16 doubled pixels for 2x modes. */
static uint32_t s_expand2[4][256];
static uint32_t s_rgb332[256];

void render332_init(void) {
    memset(s_expand2, 0, sizeof(s_expand2));
    for (int bits = 0; bits < 256; bits++) {
        uint64_t mask1 = 0;
        for (int i = 0; i < 8; i++) {
            if (bits & (1 << i)) {
                mask1 |= (uint64_t)0xffu << (i * 8);
            }
        }
        s_expand1[bits] = mask1;

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
}

bool render332_is_2x(const video_state_t *v) {
    return v->mode != VIDEO_MODE_TEXT80 && v->mode != VIDEO_MODE_TEXT80C;
}

/* Refresh the palette LUT from the state's RGB888 palette. Cached by
 * the state version so the 256-entry conversion runs only when the
 * palette actually changes, not once per rendered line. */
static void update_palette(const video_state_t *v) {
    static uint32_t cached_version;
    static bool valid;

    if (valid && cached_version == v->version) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        uint32_t c = v->palette[i];
        uint32_t px = render332_rgb(c);
        s_rgb332[i] = px * 0x01010101u;
    }
    cached_version = v->version;
    valid = true;
}

static void put_cell_1x(uint32_t *dst, uint8_t bits, uint32_t fg,
                        uint32_t bg) {
    uint64_t mask = s_expand1[bits];
    uint32_t left = (uint32_t)mask;
    uint32_t right = (uint32_t)(mask >> 32);
    dst[0] = (fg & left) | (bg & ~left);
    dst[1] = (fg & right) | (bg & ~right);
}

static void put_cell_2x(uint32_t *dst, uint8_t bits, uint32_t fg,
                        uint32_t bg) {
    for (int word = 0; word < 4; word++) {
        uint32_t mask = s_expand2[word][bits];
        dst[word] = (fg & mask) | (bg & ~mask);
    }
}

void render_line_332(const video_state_t *v, int y, uint32_t *out) {
    update_palette(v);

    int mode = v->mode;
    int scale = render332_is_2x(v) ? 2 : 1;
    int ly = y / scale;
    int row = ly / 8;
    int sub = ly % 8;

    if (mode == VIDEO_MODE_PIXEL) {
        const uint8_t *fb = v->framebuf ? v->framebuf + row * VIDEO_FB_COLS : NULL;
        for (int x = 0; x < VIDEO_FB_COLS; x += 2) {
            uint8_t a = fb ? fb[x] : 0;
            uint8_t b = fb ? fb[x + 1] : 0;
            uint32_t px = s_rgb332[a] & 0xffffu;
            uint32_t px2 = s_rgb332[b] & 0xffffu;
            *out++ = px | (px2 << 16);
        }
        return;
    }

    int cols = (scale == 1) ? 80 : 40;
    for (int col = 0; col < cols; col++) {
        uint8_t ch = v->char_map[0][row * cols + col];
        uint8_t attr = v->attr_map[0][row * cols + col];
        for (int layer = VIDEO_LAYERS - 1; layer > 0; layer--) {
            if (!v->layer_active[layer]) {
                continue;
            }
            uint8_t candidate = v->attr_map[layer][row * cols + col];
            if ((candidate & VIDEO_ATTR_TRANSPARENT) == 0) {
                ch = v->char_map[layer][row * cols + col];
                attr = candidate;
                break;
            }
        }
        uint8_t bits = v->tile_defined[ch] ? v->tiles[ch][sub]
                                           : (uint8_t)font8x8_basic[ch & 0x7f][sub];

        uint8_t fg_idx, bg_idx;
        if (mode == VIDEO_MODE_TEXT40C || mode == VIDEO_MODE_TEXT80C) {
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
        if (scale == 1) {
            put_cell_1x(out, bits, fg, bg);
            out += 2;
        } else {
            put_cell_2x(out, bits, fg, bg);
            out += 4;
        }
    }
}

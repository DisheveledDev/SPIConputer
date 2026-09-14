/* render332.c — RGB332 fast scanline renderer; see render332.h */
#include "render332.h"

#include <stdbool.h>
#include <string.h>

#include "font8x8_basic.h"

/* Core 0's render pump calls this for every row, so on the product
 * board the code and every table it reads live in SRAM: the XIP cache is
 * shared with core 1, and a miss behind a Lua program's traffic stalls
 * core 0 long enough to delay the scanout's DMA IRQ (see video_hw.c). */
#if defined(PICO_RP2040) || defined(PICO_RP2350)
#include "pico.h"
#define RENDER_HOT(name) __not_in_flash_func(name)
#else
#define RENDER_HOT(name) name
#endif

/* Four output words containing 16 doubled pixels (one 8-pixel tile row
 * scaled 2x horizontally). */
static uint32_t s_expand2[4][256];
static uint32_t s_rgb332[256];
static bool s_palette_valid;

/* SRAM copy of the ROM font: the compiler places the never-written
 * font8x8_basic table in flash. */
static uint8_t s_font[128][8];

void render332_init(void) {
    memcpy(s_font, font8x8_basic, sizeof(s_font));
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

void RENDER_HOT(render332_invalidate_palette)(void) {
    s_palette_valid = false;
}

/* Refresh the palette LUT from the state's RGB888 palette. Cached until
 * render332_invalidate_palette() (called after a queue drain that
 * changed the palette), not once per rendered line. */
static void RENDER_HOT(update_palette)(const video_state_t *v) {
    if (s_palette_valid) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        s_rgb332[i] = render332_rgb(v->palette[i]) * 0x01010101u;
    }
    s_palette_valid = true;
}

static void RENDER_HOT(put_cell_2x)(uint32_t *dst, uint8_t bits,
                                    uint32_t fg, uint32_t bg) {
    for (int word = 0; word < 4; word++) {
        uint32_t mask = s_expand2[word][bits];
        dst[word] = (fg & mask) | (bg & ~mask);
    }
}

void RENDER_HOT(render_line_332)(const video_state_t *v, int ly,
                                 uint32_t *out) {
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
        int idx = row * VIDEO_COLS + col;
        uint8_t ch = v->base_char[idx];
        uint8_t attr = v->base_attr[idx];
        uint8_t oattr = v->overlay_attr[idx];
        if ((oattr & VIDEO_ATTR_TRANSPARENT) == 0) {
            ch = v->overlay_char[idx];
            attr = oattr;
        }
        uint8_t bits = v->tile_defined[ch] ? v->tiles[ch][sub]
                                           : s_font[ch & 0x7f][sub];

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

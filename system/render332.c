/* render332.c — RGB332 fast scanline renderer; see render332.h */
#include "render332.h"

#include <stdbool.h>
#include <string.h>


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
 * scaled 2x horizontally), and two words containing the 8 pixels at 1x
 * for the 80-column modes. */
static uint32_t s_expand2[4][256];
static uint32_t s_expand1[2][256];
static uint32_t s_rgb332[256];
static bool s_palette_valid;

void render332_init(void) {
    /* The SRAM copy of the ROM font (video.c): the const table lives in
     * flash, which core 0 must not read. */
    video_font_init();
    memset(s_expand2, 0, sizeof(s_expand2));
    memset(s_expand1, 0, sizeof(s_expand1));
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
        for (int word = 0; word < 2; word++) {
            uint32_t mask1 = 0;
            for (int pixel = 0; pixel < 4; pixel++) {
                if (bits & (1 << (word * 4 + pixel))) {
                    mask1 |= 0xffu << (pixel * 8);
                }
            }
            s_expand1[word][bits] = mask1;
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

/* One row of mode 10 palette bytes, every pixel doubled: a straight LUT
 * walk of the byte row. */
static void RENDER_HOT(put_pixel_row)(const uint8_t *fb, uint32_t *out) {
    for (int x = 0; x < VIDEO_FB_COLS; x += 2) {
        uint8_t a = fb ? fb[x] : 0;
        uint8_t b = fb ? fb[x + 1] : 0;
        uint32_t px = s_rgb332[a] & 0xffffu;
        uint32_t px2 = s_rgb332[b] & 0xffffu;
        *out++ = px | (px2 << 16);
    }
}

static void RENDER_HOT(put_cell_1x)(uint32_t *dst, uint8_t bits,
                                    uint32_t fg, uint32_t bg) {
    uint32_t mask = s_expand1[0][bits];
    dst[0] = (fg & mask) | (bg & ~mask);
    mask = s_expand1[1][bits];
    dst[1] = (fg & mask) | (bg & ~mask);
}

/* Budget: a 40-column row is 40 cells for two output lines (63.5 us),
 * an 80-column row 80 cells for one (31.7 us), so the 1x modes ask
 * core 0 for four times the cells per line: about 8 loads, two LUT
 * reads and two stores per cell, ~20 cycles, so ~13 us of the 31.7 us
 * line at 126 MHz, inside the ring's slack (video_hw.c reports the
 * measured row times). */
void RENDER_HOT(render_line_332)(const video_state_t *v, int ly,
                                 uint32_t *out) {
    update_palette(v);

    int row = ly / 8;
    int sub = ly % 8;

    if (v->mode == VIDEO_MODE_PIXEL) {
        /* Direct pixels: one logical line per output line pair. */
        put_pixel_row(v->framebuf ? v->framebuf + ly * VIDEO_FB_COLS : NULL, out);
        return;
    }

    if (v->mode == VIDEO_MODE_PIXEL_LO) {
        /* 160x120: each byte is one output word (4 pixels, the LUT
         * entry is the colour replicated), each buffer row two logical
         * rows. 160 loads and stores a line: the cheapest mode. */
        const uint8_t *fb = v->framebuf ? v->framebuf + (ly / 2) * VIDEO_FB_LO_COLS : NULL;
        for (int x = 0; x < VIDEO_FB_LO_COLS; x++) {
            *out++ = s_rgb332[fb ? fb[x] : 0];
        }
        return;
    }

    int cols = video_mode_cols(v->mode);
    bool x2 = video_mode_2x(v->mode);
    bool colour_mode = video_mode_colour(v->mode);
    for (int col = 0; col < cols; col++) {
        int idx = row * cols + col;
        uint8_t ch = v->base_char[idx];
        uint8_t attr = v->base_attr[idx];
        uint8_t oattr = v->overlay_attr[idx];
        if ((oattr & VIDEO_ATTR_TRANSPARENT) == 0) {
            ch = v->overlay_char[idx];
            attr = oattr;
        }
        uint8_t bits = v->tile_defined[ch] ? v->tiles[ch][sub]
                                           : video_font[ch][sub];

        uint8_t fg_idx, bg_idx;
        if (colour_mode) {
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
        if (x2) {
            put_cell_2x(out, bits, fg, bg);
            out += 4;
        } else {
            put_cell_1x(out, bits, fg, bg);
            out += 2;
        }
    }
}

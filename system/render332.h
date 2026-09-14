/* render332.h
 *
 * RGB332 scanline renderer for the HSTX scanout (Phase 7).
 *
 * Platform-neutral: produces the exact 32-bit words the HSTX TMDS
 * expander consumes, so it runs under the host tests like render.c.
 * render.c remains the RGB888 reference implementation; a host test
 * checks this module against it for every mode.
 *
 * Pixel format (matches the pico-examples HSTX DVI demo and the
 * expand_tmds ROT/NBITS setup): one byte per pixel, BBBGGGRR, little
 * endian within each 32-bit word (pixel 0 in bits 7:0).
 */
#pragma once

#include <stdint.h>

#include "render.h"
#include "video.h"

/* Output words per 640-pixel line. */
#define RENDER332_WORDS_PER_LINE (RENDER_OUT_WIDTH / 4)

/* Build the expansion tables. Call once from the render owner (core 0
 * on the product board, test setup on the host) before rendering. */
void render332_init(void);

/* Render output line y (0..479) into out (RENDER332_WORDS_PER_LINE
 * words). Same picture as render_line() with the palette reduced to
 * RGB332. `framebuf` NULL (mode 10 mid-switch) renders black. */
void render_line_332(const video_state_t *v, int y, uint32_t *out);

/* Output lines that repeat a logical row (2x-scaled modes), so a
 * scanout can render each row once and show it twice. */
bool render332_is_2x(const video_state_t *v);

/* RGB332 value for an RGB888 colour, exposed for palette building and
 * the host colour-order test. R in bits 7:5, G in 4:2, B in 1:0. */
static inline uint8_t render332_rgb(uint32_t rgb888) {
    uint8_t r = (uint8_t)(rgb888 >> 16);
    uint8_t g = (uint8_t)(rgb888 >> 8);
    uint8_t b = (uint8_t)rgb888;
    return (uint8_t)((r & 0xe0u) | ((g & 0xe0u) >> 3) | ((b & 0xc0u) >> 6));
}

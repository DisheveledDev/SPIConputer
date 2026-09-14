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

/* Drop the cached RGB332 palette LUT: the next line rebuilds it from
 * the state's palette. Core 0 calls this after a drain that changed the
 * palette (see video_ops_drain). */
void render332_invalidate_palette(void);

/* Render logical row ly (0..VIDEO_ROWS-1) into out
 * (RENDER332_WORDS_PER_LINE words). Same picture as render_line() with
 * the palette reduced to RGB332; every pixel is doubled horizontally.
 * `framebuf` NULL (mode 10 mid-switch) renders black. */
void render_line_332(const video_state_t *v, int ly, uint32_t *out);

/* RGB332 value for an RGB888 colour, exposed for palette building and
 * the host colour-order test. R in bits 7:5, G in 4:2, B in 1:0. */
static inline uint8_t render332_rgb(uint32_t rgb888) {
    uint8_t r = (uint8_t)(rgb888 >> 16);
    uint8_t g = (uint8_t)(rgb888 >> 8);
    uint8_t b = (uint8_t)rgb888;
    return (uint8_t)((r & 0xe0u) | ((g & 0xe0u) >> 3) | ((b & 0xc0u) >> 6));
}

/* render.h
 *
 * Platform-neutral scanline renderer (Phase 1). Renders one 640-pixel
 * RGB888 output line at a time from a video_state_t, exactly as core 0
 * will feed HSTX on the product board. Fixed 640x480 output for all
 * modes; the 320x240 logical modes (0/1/10) are scaled 2x.
 */
#pragma once

#include <stdint.h>

#include "video.h"

#define RENDER_OUT_WIDTH 640
#define RENDER_OUT_HEIGHT 480
#define RENDER_LINE_BYTES (RENDER_OUT_WIDTH * 3)

/* Render output line y (0..479) into out (RENDER_LINE_BYTES bytes,
 * RGB888). Tile modes fetch tiles from the ROM font (ASCII-aligned)
 * or the state's override set; attribute bytes carry invert + colour.
 * Mode 10 streams the framebuffer through the palette. */
void render_line(const video_state_t *v, int y, uint8_t *out);

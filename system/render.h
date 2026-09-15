/* render.h
 *
 * Platform-neutral scanline renderer (Phase 1). Renders one 640-pixel
 * RGB888 output line at a time from a video_state_t, exactly as core 0
 * will feed HSTX on the product board. The 2x modes (40x30 tiles,
 * 320x240 pixels) are scaled 2x horizontally and vertically, so the
 * caller passes a logical row (0..239) and gets the 640-pixel line that
 * scans out as that row's first output line (the second is identical).
 * The 1x modes (80x60 tiles) map one logical row to one output line,
 * so the caller passes 0..479 (video_mode_lines gives the count).
 */
#pragma once

#include <stdint.h>

#include "video.h"

#define RENDER_OUT_WIDTH 640
#define RENDER_OUT_HEIGHT 480
#define RENDER_LINE_BYTES (RENDER_OUT_WIDTH * 3)

/* Render logical row ly (0..video_mode_lines(mode)-1) into out
 * (RENDER_LINE_BYTES bytes, RGB888). Tile modes fetch tiles from the
 * ROM font (ASCII-aligned) or the state's override set; attribute bytes
 * carry invert + colour. Tile rows are bit-packed with bit 0 as the
 * leftmost pixel. Mode 10 streams the pixel buffer through the
 * palette. */
void render_line(const video_state_t *v, int ly, uint8_t *out);

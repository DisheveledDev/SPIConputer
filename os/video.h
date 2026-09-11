/* video.h
 *
 * Video state (Phase 1). Each program owns its video state (heap-
 * allocated by program.c), so save/restore on the program stack is a
 * pointer swap of g_current_video. The scanline renderer (render.c)
 * and the serial mirror (core0/serial_mirror.c) read the current
 * state; the screen Lua module (screen_lua.c) is the writer.
 *
 * Current scope: single-buffered state with a version counter. The
 * renderer tolerates mid-frame updates (bounded tearing); HSTX output
 * with double-buffered maps + vsync swaps lands with the product board
 * bring-up (Phase 7).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VIDEO_COLS 80
#define VIDEO_ROWS 60
#define VIDEO_FB_COLS 320
#define VIDEO_FB_ROWS 240

#define VIDEO_MODE_TEXT40 0 /* 40x30, B&W, 2x scaled */
#define VIDEO_MODE_TEXT40C 1 /* 40x30, per-cell invert + colour */
#define VIDEO_MODE_TEXT80 2 /* 80x60, B&W */
#define VIDEO_MODE_TEXT80C 3 /* 80x60, per-cell invert + colour */
#define VIDEO_MODE_PIXEL 10 /* 320x240 direct pixels, 256-entry palette */

typedef struct {
    uint8_t mode; /* VIDEO_MODE_* */

    /* Tile modes 0/1/2/3. The maps are 80x60 (worst case); the active
     * area depends on the mode. */
    uint8_t char_map[VIDEO_COLS * VIDEO_ROWS];
    uint8_t attr_map[VIDEO_COLS * VIDEO_ROWS];
    /* Attribute byte: bit 7 invert; bits 0-2 colour index.
     * Colour index c uses palette entry c+1 (0 = default white). */

    /* RAM tile override set; ROM font (font8x8, ASCII-aligned) used
     * where tile_defined[i] == 0. Each tile is 8 row bytes; bit 0 of a
     * row byte is the leftmost pixel (same as the ROM font). */
    uint8_t tiles[256][8];
    uint8_t tile_defined[256];

    /* Mode 10 only: 320x240 byte framebuffer, indexes into palette. */
    uint8_t *framebuf;

    uint32_t palette[256]; /* RGB888 */

    /* Bumped by the screen module on every mutation; the serial
     * mirror uses it to detect changes (headers + tile diffs). */
    uint32_t version;
} video_state_t;

/* The active video state: points at the top program's video state.
 * Written only by program push/pop (core 1); read by the renderer and
 * the serial mirror (core 0). */
extern video_state_t *g_current_video;

/* Init a fresh state: mode 0, cleared maps, C64-ish 16-entry palette
 * for the low indexes, version 0. */
void video_state_init(video_state_t *v);

/* Release dynamic buffers (mode 10 framebuffer). */
void video_state_free(video_state_t *v);

/* Active logical dimensions for a mode. */
int video_mode_cols(int mode);
int video_mode_rows(int mode);

/* Switch modes: clears maps (or allocates the mode 10 framebuffer),
 * bumps the version. Returns false on invalid mode or OOM. */
bool video_set_mode(video_state_t *v, int mode);

/* Character dimensions (text modes only). */
int video_char_cols(const video_state_t *v);
int video_char_rows(const video_state_t *v);

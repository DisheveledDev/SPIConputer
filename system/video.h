/* video.h
 *
 * Video state (Phase 1). Each program owns its video state (heap-
 * allocated by program.c), so save/restore on the program stack is a
 * pointer swap of g_current_video. The scanline renderer (render.c)
 * state; the screen Lua module (screen_lua.c) is the writer.
 *
 * Current scope: single-buffered state with a version counter. The
 * renderer tolerates mid-frame updates (bounded tearing); HSTX output
 * with double-buffered maps + vsync swaps lands with the product board
 * bring-up (Phase 7).
 */
#pragma once

#include <stdatomic.h>
#include <stddef.h>
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
#define VIDEO_LAYERS 3
#define VIDEO_ATTR_TRANSPARENT 0x40

typedef struct {
    uint8_t mode; /* VIDEO_MODE_* */
    uint8_t z_order;
    uint8_t layer_active[VIDEO_LAYERS];

    /* Tile modes 0/1/2/3. Layer 0 is the base, layers 1 and 2 are
     * transparent overlays. The maps are 80x60 (worst case); the active
     * area depends on the mode. */
    uint8_t char_map[VIDEO_LAYERS][VIDEO_COLS * VIDEO_ROWS];
    uint8_t attr_map[VIDEO_LAYERS][VIDEO_COLS * VIDEO_ROWS];
    /* Attribute byte: bit 7 invert; bits 0-2 colour index; bit 6 makes
     * overlay cells transparent. Colour index c uses palette entry c+1. */

    /* RAM tile override set; ROM font (font8x8, ASCII-aligned) used
     * where tile_defined[i] == 0. Each tile is 8 row bytes; bit 0 of a
     * row byte is the leftmost pixel (same as the ROM font). */
    uint8_t tiles[256][8];
    uint8_t tile_defined[256];

    /* Mode 10 only: 320x240 byte framebuffer, indexes into palette. */
    uint8_t *framebuf;

    uint32_t palette[256]; /* RGB888 */

    /* Even version = stable. A mutation changes this to odd before
     * writing and back to the next even value after writing. The update
     * flag blocks Lua mutations while core 0 copies a frame snapshot. */
    volatile uint32_t version;
    atomic_flag update_lock;
} video_state_t;

/* The active video state: points at the top program's video state.
 * Published by the OS core and read by the video core. The atomic pointer
 * makes the cross-core publication explicit; the retire queue keeps the
 * pointed-to state alive for two frame boundaries. */
extern _Atomic(video_state_t *) g_current_video;

static inline video_state_t *video_current_load(void) {
    return atomic_load_explicit(&g_current_video, memory_order_acquire);
}

static inline void video_current_store(video_state_t *video) {
    atomic_store_explicit(&g_current_video, video, memory_order_release);
}

/* Init a fresh state: mode 0, cleared maps, C64-ish 16-entry palette
 * for the low indexes, version 0. */
void video_state_init(video_state_t *v);

/* Release dynamic buffers (mode 10 framebuffer). */
void video_state_free(video_state_t *v);

/* Begin/end a core-1 mutation. The video core's snapshot copy takes the
 * same flag, so screen updates block briefly while a frame is copied. */
static inline void video_state_begin_mutation(video_state_t *v) {
    while (atomic_flag_test_and_set_explicit(&v->update_lock,
                                              memory_order_acquire)) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    v->version++;
}

static inline void video_state_end_mutation(video_state_t *v) {
    v->version++;
    atomic_flag_clear_explicit(&v->update_lock, memory_order_release);
}

/* Copy a stable source state into a core-0 snapshot. Returns false if
 * core 1 currently owns the update lock; core 0 skips that frame rather
 * than spinning inside the real-time DMA IRQ. */
bool video_state_snapshot_copy(const video_state_t *src, video_state_t *dst,
                               uint8_t *dst_framebuf,
                               size_t dst_framebuf_size);

/* Active logical dimensions for a mode. */
int video_mode_cols(int mode);
int video_mode_rows(int mode);

/* Switch modes: clears maps (or allocates the mode 10 framebuffer),
 * bumps the version. Returns false on invalid mode or OOM. */
bool video_set_mode(video_state_t *v, int mode);

/* Character dimensions (text modes only). */
int video_char_cols(const video_state_t *v);
int video_char_rows(const video_state_t *v);

/* Select the layer mutated by the Screen* text APIs. */
bool video_set_z_order(video_state_t *v, int layer);

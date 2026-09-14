/* video.c — see video.h */
#include "video.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

_Atomic(video_state_t *) g_current_video;

void video_state_init(video_state_t *v) {
    memset(v, 0, sizeof(*v));
    atomic_flag_clear(&v->update_lock);
    v->z_order = 0;
    v->layer_active[0] = 1;
    for (int layer = 1; layer < VIDEO_LAYERS; layer++) {
        memset(v->attr_map[layer], VIDEO_ATTR_TRANSPARENT,
               sizeof(v->attr_map[layer]));
    }
    /* Palette: C64-ish 16-entry colours for the low indexes; entries
     * above stay black until ScreenPalette sets them. */
    static const uint32_t c64[16] = {
        0x000000, 0xffffff, 0x880000, 0xaaffee,
        0xcc44cc, 0x00cc55, 0x0000aa, 0xeeee77,
        0xdd8855, 0x664400, 0xff7777, 0x333333,
        0x777777, 0xaaff66, 0x0088ff, 0xbbbbbb,
    };
    for (int i = 0; i < 16; i++) {
        v->palette[i] = c64[i];
    }
}

bool video_state_snapshot_copy(const video_state_t *src, video_state_t *dst,
                               uint8_t *dst_framebuf,
                               size_t dst_framebuf_size) {
    video_state_t *mutable_src = (video_state_t *)src;
    if (atomic_flag_test_and_set_explicit(&mutable_src->update_lock,
                                          memory_order_acquire)) {
        return false;
    }

    uint8_t *source_framebuf = mutable_src->framebuf;
    memcpy(dst, mutable_src, offsetof(video_state_t, version));
    dst->framebuf = NULL;
    if (source_framebuf && dst_framebuf &&
        dst_framebuf_size >= VIDEO_FB_COLS * VIDEO_FB_ROWS) {
        memcpy(dst_framebuf, source_framebuf, VIDEO_FB_COLS * VIDEO_FB_ROWS);
        dst->framebuf = dst_framebuf;
    }
    dst->version = mutable_src->version;
    atomic_flag_clear_explicit(&mutable_src->update_lock, memory_order_release);
    return true;
}

void video_state_free(video_state_t *v) {
    free(v->framebuf);
    v->framebuf = NULL;
}

int video_mode_cols(int mode) {
    if (mode == VIDEO_MODE_PIXEL) return VIDEO_FB_COLS;
    if (mode == VIDEO_MODE_TEXT80 || mode == VIDEO_MODE_TEXT80C) return 80;
    return 40;
}

int video_mode_rows(int mode) {
    if (mode == VIDEO_MODE_PIXEL) return VIDEO_FB_ROWS;
    if (mode == VIDEO_MODE_TEXT80 || mode == VIDEO_MODE_TEXT80C) return 60;
    return 30;
}

bool video_set_mode(video_state_t *v, int mode) {
    if (mode != VIDEO_MODE_TEXT40 && mode != VIDEO_MODE_TEXT40C &&
        mode != VIDEO_MODE_TEXT80 && mode != VIDEO_MODE_TEXT80C &&
        mode != VIDEO_MODE_PIXEL) {
        return false;
    }
    video_state_begin_mutation(v);
    v->z_order = 0;
    v->layer_active[0] = 1;
    for (int layer = 1; layer < VIDEO_LAYERS; layer++) {
        v->layer_active[layer] = 0;
    }
    memset(v->char_map, ' ', sizeof(v->char_map));
    memset(v->attr_map, 0, sizeof(v->attr_map));
    for (int layer = 1; layer < VIDEO_LAYERS; layer++) {
        memset(v->attr_map[layer], VIDEO_ATTR_TRANSPARENT,
               sizeof(v->attr_map[layer]));
    }
    if (mode == VIDEO_MODE_PIXEL) {
        /* Allocate before publishing the mode: core 0's renderer must
         * never observe mode 10 without a framebuffer. */
        if (!v->framebuf) {
            uint8_t *fb = (uint8_t *)malloc(VIDEO_FB_COLS * VIDEO_FB_ROWS);
            if (!fb) {
                video_state_end_mutation(v);
                return false;
            }
            memset(fb, 0, VIDEO_FB_COLS * VIDEO_FB_ROWS);
            v->framebuf = fb;
        } else {
            memset(v->framebuf, 0, VIDEO_FB_COLS * VIDEO_FB_ROWS);
        }
    } else {
        /* Leaving the pixel mode: publish the new mode first (with a
         * barrier), then release the framebuffer. */
        v->mode = (uint8_t)mode;
        atomic_thread_fence(memory_order_seq_cst);
        video_state_free(v);
        video_state_end_mutation(v);
        return true;
    }
    v->mode = (uint8_t)mode;
    atomic_thread_fence(memory_order_seq_cst);
    video_state_end_mutation(v);
    return true;
}
int video_char_cols(const video_state_t *v) {
    if (v->mode == VIDEO_MODE_TEXT80 || v->mode == VIDEO_MODE_TEXT80C) {
        return 80;
    }
    return 40;
}

int video_char_rows(const video_state_t *v) {
    if (v->mode == VIDEO_MODE_TEXT80 || v->mode == VIDEO_MODE_TEXT80C) {
        return 60;
    }
    return 30;
}

bool video_set_z_order(video_state_t *v, int layer) {
    if (layer < 0 || layer >= VIDEO_LAYERS ||
        v->mode == VIDEO_MODE_PIXEL) {
        return false;
    }
    video_state_begin_mutation(v);
    v->z_order = (uint8_t)layer;
    video_state_end_mutation(v);
    return true;
}

/* video.c — see video.h */
#include "video.h"

#include <stdlib.h>
#include <string.h>

video_state_t *g_current_video;

void video_state_init(video_state_t *v) {
    memset(v, 0, sizeof(*v));
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
    v->mode = (uint8_t)mode;
    memset(v->char_map, ' ', sizeof(v->char_map));
    memset(v->attr_map, 0, sizeof(v->attr_map));
    if (mode == VIDEO_MODE_PIXEL) {
        if (!v->framebuf) {
            v->framebuf = (uint8_t *)malloc(VIDEO_FB_COLS * VIDEO_FB_ROWS);
            if (!v->framebuf) {
                return false;
            }
        }
        memset(v->framebuf, 0, VIDEO_FB_COLS * VIDEO_FB_ROWS);
    } else {
        video_state_free(v);
    }
    v->version++;
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

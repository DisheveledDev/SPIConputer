/* video.c — see video.h */
#include "video.h"

#include <stdatomic.h>
#include <string.h>

/* One pixel buffer, shared by whichever slot holds mode 10. Only one
 * program can use the pixel mode at a time (Launch from a mode-10
 * program is refused), so two slots never need it at once. Core 0 owns
 * it, like the slots themselves. */
static uint8_t s_pixel_buffer[VIDEO_FB_COLS * VIDEO_FB_ROWS];

static video_state_t s_screens[VIDEO_SLOTS];
static int s_screen_active;

/* Single-producer/single-consumer op ring. The indices are free-running
 * and masked on use; the producer only ever writes s_tail and the
 * consumer only s_head. The cores share SRAM with no caches between
 * them, so plain volatile words are enough, as elsewhere. */
static video_op_t s_queue[VIDEO_QUEUE_OPS];
static volatile uint32_t s_head; /* consumer: core 0 */
static volatile uint32_t s_tail; /* producer: core 1 */

/* Core-1-side shadow of the mode Lua last requested. */
static uint8_t s_lua_mode;

void video_state_init(video_state_t *v) {
    memset(v, 0, sizeof(*v));
    /* The overlay starts fully transparent (every cell hidden). */
    memset(v->overlay_attr, VIDEO_ATTR_TRANSPARENT, sizeof(v->overlay_attr));
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

void video_op_put(const video_op_t *op) {
    /* Full queue: wait for core 0's frame-boundary drain. A dead video
     * core blocks here, which stops the watchdog feeds and resets the
     * board rather than running on with a frozen display. */
    while ((uint32_t)(s_tail - s_head) >= VIDEO_QUEUE_OPS) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    s_queue[s_tail % VIDEO_QUEUE_OPS] = *op;
    atomic_signal_fence(memory_order_seq_cst);
    s_tail++;
}

void video_note_mode(int mode) {
    s_lua_mode = (uint8_t)mode;
}

int video_lua_mode(void) {
    return s_lua_mode;
}

void video_screens_init(void) {
    for (int i = 0; i < VIDEO_SLOTS; i++) {
        video_state_init(&s_screens[i]);
    }
    s_screen_active = 0;
    s_head = 0;
    s_tail = 0;
    s_lua_mode = VIDEO_MODE_TEXT40;
}

video_state_t *video_screen(void) {
    return &s_screens[s_screen_active];
}

int video_screen_index(void) {
    return s_screen_active;
}

bool video_mode_valid(int mode) {
    return mode == VIDEO_MODE_TEXT40 || mode == VIDEO_MODE_TEXT40C ||
           mode == VIDEO_MODE_PIXEL;
}

int video_mode_cols(int mode) {
    if (mode == VIDEO_MODE_PIXEL) return VIDEO_FB_COLS;
    return VIDEO_COLS;
}

int video_mode_rows(int mode) {
    if (mode == VIDEO_MODE_PIXEL) return VIDEO_FB_ROWS;
    return VIDEO_ROWS;
}

/* Clear the maps and the frame geometry for a new mode; entering the
 * pixel mode attaches and clears the shared pixel buffer. */
static void apply_mode(video_state_t *v, int mode) {
    memset(v->base_char, ' ', sizeof(v->base_char));
    memset(v->base_attr, 0, sizeof(v->base_attr));
    memset(v->overlay_char, 0, sizeof(v->overlay_char));
    memset(v->overlay_attr, VIDEO_ATTR_TRANSPARENT, sizeof(v->overlay_attr));
    if (mode == VIDEO_MODE_PIXEL) {
        memset(s_pixel_buffer, 0, sizeof(s_pixel_buffer));
        v->framebuf = s_pixel_buffer;
    } else {
        v->framebuf = NULL;
    }
    v->mode = (uint8_t)mode;
}

static void apply_clear(video_state_t *v, int ch) {
    if (v->mode == VIDEO_MODE_PIXEL) {
        if (v->framebuf) {
            memset(v->framebuf, (uint8_t)ch, VIDEO_FB_COLS * VIDEO_FB_ROWS);
        }
        return;
    }
    memset(v->base_char, (uint8_t)ch, sizeof(v->base_char));
    memset(v->base_attr, 0, sizeof(v->base_attr));
}

/* Hide the whole overlay again: blank chars, every cell transparent. */
static void apply_over_clear(video_state_t *v, int ch) {
    if (v->mode == VIDEO_MODE_PIXEL) {
        return;
    }
    memset(v->overlay_char, (uint8_t)ch, sizeof(v->overlay_char));
    memset(v->overlay_attr, VIDEO_ATTR_TRANSPARENT, sizeof(v->overlay_attr));
}

/* Apply one op. Returns true when the palette changed. */
static bool apply_op(video_state_t *v, const video_op_t *op) {
    switch (op->op) {
        case VIDEO_OP_RESET:
            video_state_init(v);
            return true;
        case VIDEO_OP_MODE:
            if (video_mode_valid(op->a)) {
                apply_mode(v, op->a);
            }
            return false;
        case VIDEO_OP_OUT:
            if (op->a < VIDEO_COLS && op->b < VIDEO_ROWS &&
                v->mode != VIDEO_MODE_PIXEL) {
                int idx = op->b * VIDEO_COLS + op->a;
                v->base_char[idx] = op->c;
                v->base_attr[idx] = (uint8_t)op->d;
            }
            return false;
        case VIDEO_OP_ATTR:
            if (op->a < VIDEO_COLS && op->b < VIDEO_ROWS &&
                v->mode != VIDEO_MODE_PIXEL) {
                v->base_attr[op->b * VIDEO_COLS + op->a] = (uint8_t)op->d;
            }
            return false;
        case VIDEO_OP_CLEAR:
            apply_clear(v, op->a);
            return false;
        case VIDEO_OP_OVER_OUT:
            if (op->a < VIDEO_COLS && op->b < VIDEO_ROWS &&
                v->mode != VIDEO_MODE_PIXEL) {
                int idx = op->b * VIDEO_COLS + op->a;
                v->overlay_char[idx] = op->c;
                v->overlay_attr[idx] = (uint8_t)op->d;
            }
            return false;
        case VIDEO_OP_OVER_ATTR:
            if (op->a < VIDEO_COLS && op->b < VIDEO_ROWS &&
                v->mode != VIDEO_MODE_PIXEL) {
                v->overlay_attr[op->b * VIDEO_COLS + op->a] = (uint8_t)op->d;
            }
            return false;
        case VIDEO_OP_OVER_CLEAR:
            apply_over_clear(v, op->a);
            return false;
        case VIDEO_OP_TILE:
            for (int i = 0; i < 4; i++) {
                v->tiles[op->a][i] = (uint8_t)(op->d >> (i * 8));
                v->tiles[op->a][4 + i] = (uint8_t)(op->e >> (i * 8));
            }
            v->tile_defined[op->a] = 1;
            return false;
        case VIDEO_OP_PALETTE:
            v->palette[op->a] = op->d & 0xffffffu;
            return true;
        case VIDEO_OP_PLOT:
            /* a (x) is a uint8_t, so x < 320 always holds. */
            if (v->mode == VIDEO_MODE_PIXEL && v->framebuf &&
                op->b < VIDEO_FB_ROWS) {
                v->framebuf[op->b * VIDEO_FB_COLS + op->a] = (uint8_t)op->d;
            }
            return false;
        default:
            return false; /* VIDEO_OP_SLOT is handled by the drain */
    }
}

bool video_ops_drain(void) {
    bool palette_changed = false;
    uint32_t tail = s_tail;

    while (s_head != tail) {
        /* Copy before publishing the head: the producer may overwrite
         * the slot as soon as it sees the advance. */
        video_op_t op = s_queue[s_head % VIDEO_QUEUE_OPS];
        s_head++;
        if (op.op == VIDEO_OP_SLOT) {
            if (op.a < VIDEO_SLOTS) {
                s_screen_active = op.a;
            }
        } else if (apply_op(&s_screens[s_screen_active], &op)) {
            palette_changed = true;
        }
    }
    return palette_changed;
}

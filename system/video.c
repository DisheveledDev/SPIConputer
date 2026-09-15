/* video.c — see video.h */
#include "video.h"

#include <stdatomic.h>
#include <stddef.h>

/* The drain runs on core 0 once per frame, between rows of the render
 * pump, so on the product board it and everything it calls live in
 * SRAM: the XIP cache is shared with core 1, and a miss there stalls
 * core 0 long enough to delay the scanout's DMA IRQ (see video_hw.c). */
#if defined(PICO_RP2040) || defined(PICO_RP2350)
#include "pico.h"
#define VIDEO_HOT(name) __not_in_flash_func(name)
#define VIDEO_HOT_DATA __not_in_flash("video_data")
#else
#define VIDEO_HOT(name) name
#define VIDEO_HOT_DATA
#endif

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
static volatile uint32_t s_drain_count;
static volatile uint32_t s_base_out_count;
static volatile uint32_t s_overlay_out_count;

/* Core-1-side shadow of the mode Lua last requested. */
static uint8_t s_lua_mode;

/* VIDEO_OP_TEXT staging (see video.h). `seq` is the queue index of the
 * op that last referenced a slot; the slot is free again once the
 * consumer's head has passed it. */
static uint8_t s_staging[VIDEO_STAGING_SLOTS][VIDEO_STAGING_BYTES];
static volatile uint32_t s_staging_seq[VIDEO_STAGING_SLOTS];
static volatile bool s_staging_busy[VIDEO_STAGING_SLOTS];
static int s_staging_next;

uint8_t *video_staging_acquire(int *slot) {
    int s = s_staging_next;
    s_staging_next = (s + 1) % VIDEO_STAGING_SLOTS;
    while (s_staging_busy[s] && (int32_t)(s_head - s_staging_seq[s]) <= 0) {
        video_queue_full_hook();
        atomic_signal_fence(memory_order_seq_cst);
    }
    s_staging_busy[s] = false;
    *slot = s;
    return s_staging[s];
}

void video_op_put_staged(const video_op_t *op) {
    /* Record the index this op will take before it is published, so the
     * slot cannot be reacquired until the drain has applied it. */
    s_staging_seq[op->c] = s_tail;
    s_staging_busy[op->c] = true;
    video_op_put(op);
}

/* memset for the core 0 paths: the library's lives in flash. The
 * volatile store keeps the compiler from turning the loop back into a
 * memset call. */
static void VIDEO_HOT(fill)(void *dst, uint8_t value, uint32_t len) {
    volatile uint8_t *p = dst;
    while (len--) {
        *p++ = value;
    }
}

void VIDEO_HOT(video_state_init)(video_state_t *v) {
    fill(v, 0, sizeof(*v));
    /* The overlay starts fully transparent (every cell hidden). */
    fill(v->overlay_attr, VIDEO_ATTR_TRANSPARENT, sizeof(v->overlay_attr));
    /* Palette: C64-ish 16-entry colours for the low indexes; entries
     * above stay black until ScreenPalette sets them. */
    static const uint32_t VIDEO_HOT_DATA c64[16] = {
        0x000000, 0xffffff, 0x880000, 0xaaffee,
        0xcc44cc, 0x00cc55, 0x0000aa, 0xeeee77,
        0xdd8855, 0x664400, 0xff7777, 0x333333,
        0x777777, 0xaaff66, 0x0088ff, 0xbbbbbb,
    };
    for (int i = 0; i < 16; i++) {
        v->palette[i] = c64[i];
    }
}

/* Called while the producer waits on a full queue. Nothing on the
 * firmware (core 0 drains on its own); the single-threaded simulator
 * overrides it to play core 0's part, otherwise a program that queues
 * more than a frame's worth of ops in one tick would spin forever. */
__attribute__((weak)) void video_queue_full_hook(void) {}
__attribute__((weak)) void video_frame_wait_hook(void) {}

void video_op_put(const video_op_t *op) {
    /* Full queue: wait for core 0's frame-boundary drain. A dead video
     * core blocks here, which stops the watchdog feeds and resets the
     * board rather than running on with a frozen display. */
    while ((uint32_t)(s_tail - s_head) >= VIDEO_QUEUE_OPS) {
        video_queue_full_hook();
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
    s_drain_count = 0;
    s_base_out_count = 0;
    s_overlay_out_count = 0;
    s_lua_mode = VIDEO_MODE_TEXT40;
    for (int i = 0; i < VIDEO_STAGING_SLOTS; i++) {
        s_staging_busy[i] = false;
        s_staging_seq[i] = 0;
    }
    s_staging_next = 0;
}

video_state_t *VIDEO_HOT(video_screen)(void) {
    return &s_screens[s_screen_active];
}

int video_screen_index(void) {
    return s_screen_active;
}

uint32_t video_ops_pending(void) {
    return (uint32_t)(s_tail - s_head);
}

uint32_t video_ops_drain_count(void) {
    return s_drain_count;
}

uint32_t video_ops_base_out_count(void) {
    return s_base_out_count;
}

uint32_t video_ops_overlay_out_count(void) {
    return s_overlay_out_count;
}

bool VIDEO_HOT(video_mode_valid)(int mode) {
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
static void VIDEO_HOT(apply_mode)(video_state_t *v, int mode) {
    fill(v->base_char, ' ', sizeof(v->base_char));
    fill(v->base_attr, 0, sizeof(v->base_attr));
    fill(v->overlay_char, 0, sizeof(v->overlay_char));
    fill(v->overlay_attr, VIDEO_ATTR_TRANSPARENT, sizeof(v->overlay_attr));
    if (mode == VIDEO_MODE_PIXEL) {
        fill(s_pixel_buffer, 0, sizeof(s_pixel_buffer));
        v->framebuf = s_pixel_buffer;
    } else {
        v->framebuf = NULL;
    }
    v->mode = (uint8_t)mode;
}

static void VIDEO_HOT(apply_clear)(video_state_t *v, int ch) {
    if (v->mode == VIDEO_MODE_PIXEL) {
        if (v->framebuf) {
            fill(v->framebuf, (uint8_t)ch, VIDEO_FB_COLS * VIDEO_FB_ROWS);
        }
        return;
    }
    fill(v->base_char, (uint8_t)ch, sizeof(v->base_char));
    fill(v->base_attr, 0, sizeof(v->base_attr));
}

/* Hide the whole overlay again: blank chars, every cell transparent. */
static void VIDEO_HOT(apply_over_clear)(video_state_t *v, int ch) {
    if (v->mode == VIDEO_MODE_PIXEL) {
        return;
    }
    fill(v->overlay_char, (uint8_t)ch, sizeof(v->overlay_char));
    fill(v->overlay_attr, VIDEO_ATTR_TRANSPARENT, sizeof(v->overlay_attr));
}

/* ---------------- block ops (text modes) ---------------- */

/* The layer a block op addresses. */
static inline uint8_t *VIDEO_HOT(layer_chars)(video_state_t *v, uint8_t flags) {
    return (flags & VIDEO_BLK_OVERLAY) ? v->overlay_char : v->base_char;
}

static inline uint8_t *VIDEO_HOT(layer_attrs)(video_state_t *v, uint8_t flags) {
    return (flags & VIDEO_BLK_OVERLAY) ? v->overlay_attr : v->base_attr;
}

/* Clip (x,y,w,h) to the screen. Returns false when nothing is left. */
static bool VIDEO_HOT(clip_rect)(int *x, int *y, int *w, int *h) {
    int x1 = *x + *w, y1 = *y + *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (x1 > VIDEO_COLS) x1 = VIDEO_COLS;
    if (y1 > VIDEO_ROWS) y1 = VIDEO_ROWS;
    *w = x1 - *x;
    *h = y1 - *y;
    return *w > 0 && *h > 0;
}

static void VIDEO_HOT(rect_fill)(video_state_t *v, uint8_t flags, int x, int y,
                                 int w, int h, uint8_t ch, uint8_t attr) {
    if (!clip_rect(&x, &y, &w, &h)) {
        return;
    }
    uint8_t *chars = layer_chars(v, flags);
    uint8_t *attrs = layer_attrs(v, flags);
    for (int j = y; j < y + h; j++) {
        int idx = j * VIDEO_COLS + x;
        if (flags & VIDEO_BLK_CHARS) fill(chars + idx, ch, (uint32_t)w);
        if (flags & VIDEO_BLK_ATTRS) fill(attrs + idx, attr, (uint32_t)w);
    }
}

static void VIDEO_HOT(apply_rect)(video_state_t *v, const video_op_t *op) {
    uint8_t flags = (uint8_t)(op->d >> 24);
    rect_fill(v, flags, op->a, op->b, op->c, (int)(op->d & 0xff),
              (uint8_t)(op->d >> 8), (uint8_t)(op->d >> 16));
}

/* Copy a w x h block from (sx,sy) to (dx,dy) on one layer, both maps.
 * Overlapping blocks are handled by choosing the row order and copying
 * each row through a temporary, so the result is as if the source had
 * been read entirely before the destination was written. */
static void VIDEO_HOT(block_copy)(video_state_t *v, uint8_t flags, int sx, int sy,
                                  int dx, int dy, int w, int h) {
    uint8_t *chars = layer_chars(v, flags);
    uint8_t *attrs = layer_attrs(v, flags);
    uint8_t row_c[VIDEO_COLS], row_a[VIDEO_COLS];
    bool backwards = dy > sy;
    for (int i = 0; i < h; i++) {
        int r = backwards ? h - 1 - i : i;
        int src = (sy + r) * VIDEO_COLS + sx;
        int dst = (dy + r) * VIDEO_COLS + dx;
        for (int c = 0; c < w; c++) {
            row_c[c] = chars[src + c];
            row_a[c] = attrs[src + c];
        }
        for (int c = 0; c < w; c++) {
            chars[dst + c] = row_c[c];
            attrs[dst + c] = row_a[c];
        }
    }
}

static void VIDEO_HOT(apply_copy)(video_state_t *v, const video_op_t *op) {
    uint8_t flags = (uint8_t)(op->d >> 24);
    int sx = op->a, sy = op->b, w = op->c, h = (int)(op->d & 0xff);
    int dx = (int)((op->d >> 8) & 0xff), dy = (int)((op->d >> 16) & 0xff);
    /* Clip so both the source and the destination are on screen. */
    if (sx + w > VIDEO_COLS) w = VIDEO_COLS - sx;
    if (dx + w > VIDEO_COLS) w = VIDEO_COLS - dx;
    if (sy + h > VIDEO_ROWS) h = VIDEO_ROWS - sy;
    if (dy + h > VIDEO_ROWS) h = VIDEO_ROWS - dy;
    if (w <= 0 || h <= 0 || sx >= VIDEO_COLS || sy >= VIDEO_ROWS ||
        dx >= VIDEO_COLS || dy >= VIDEO_ROWS) {
        return;
    }
    block_copy(v, flags, sx, sy, dx, dy, w, h);
    if (flags & VIDEO_BLK_CLEAR) {
        /* A move: blank the part of the source the block no longer
         * covers. */
        uint8_t ch = (uint8_t)op->e, attr = (uint8_t)(op->e >> 8);
        uint8_t *chars = layer_chars(v, flags);
        uint8_t *attrs = layer_attrs(v, flags);
        for (int j = sy; j < sy + h; j++) {
            for (int i = sx; i < sx + w; i++) {
                if (i >= dx && i < dx + w && j >= dy && j < dy + h) {
                    continue;
                }
                chars[j * VIDEO_COLS + i] = ch;
                attrs[j * VIDEO_COLS + i] = attr;
            }
        }
    }
}

/* Shift the contents of a region by (dx,dy); cells shifted out are
 * lost and the cells uncovered are filled. */
static void VIDEO_HOT(apply_scroll)(video_state_t *v, const video_op_t *op) {
    uint8_t flags = (uint8_t)(op->d >> 24);
    int x = op->a, y = op->b, w = op->c, h = (int)(op->d & 0xff);
    int dx = (int8_t)((op->d >> 8) & 0xff), dy = (int8_t)((op->d >> 16) & 0xff);
    uint8_t ch = (uint8_t)op->e, attr = (uint8_t)(op->e >> 8);
    if (!clip_rect(&x, &y, &w, &h)) {
        return;
    }
    if (dx <= -w || dx >= w || dy <= -h || dy >= h) {
        rect_fill(v, flags | VIDEO_BLK_CHARS | VIDEO_BLK_ATTRS, x, y, w, h, ch,
                  attr);
        return;
    }
    /* The block that survives: source rect shifted by (dx,dy). */
    int sx = dx < 0 ? x - dx : x, dxx = dx < 0 ? x : x + dx;
    int sy = dy < 0 ? y - dy : y, dyy = dy < 0 ? y : y + dy;
    int cw = w - (dx < 0 ? -dx : dx), chh = h - (dy < 0 ? -dy : dy);
    block_copy(v, flags, sx, sy, dxx, dyy, cw, chh);
    uint8_t f = flags | VIDEO_BLK_CHARS | VIDEO_BLK_ATTRS;
    if (dy > 0) rect_fill(v, f, x, y, w, dy, ch, attr);
    if (dy < 0) rect_fill(v, f, x, y + h + dy, w, -dy, ch, attr);
    if (dx > 0) rect_fill(v, f, x, y, dx, h, ch, attr);
    if (dx < 0) rect_fill(v, f, x + w + dx, y, -dx, h, ch, attr);
}

/* ROM font box-drawing codes (CP437 layout, font8x8_rom.h): corners
 * top-left, top-right, bottom-left, bottom-right, horizontal, vertical.
 * Read by core 0, so kept out of flash. */
static const uint8_t VIDEO_HOT_DATA s_box_glyphs[2][6] = {
    {0xDA, 0xBF, 0xC0, 0xD9, 0xC4, 0xB3},
    {0xC9, 0xBB, 0xC8, 0xBC, 0xCD, 0xBA},
};

static void VIDEO_HOT(apply_box)(video_state_t *v, const video_op_t *op) {
    uint8_t flags = (uint8_t)(op->d >> 24);
    int x = op->a, y = op->b, w = op->c, h = (int)(op->d & 0xff);
    int style = (int)((op->d >> 8) & 0xff);
    uint8_t attr = (uint8_t)(op->d >> 16);
    if (w < 2 || h < 2 || x >= VIDEO_COLS || y >= VIDEO_ROWS) {
        return;
    }
    const uint8_t *g = s_box_glyphs[style == 2 ? 1 : 0];
    uint8_t *chars = layer_chars(v, flags);
    uint8_t *attrs = layer_attrs(v, flags);
    int x1 = x + w - 1, y1 = y + h - 1;
    /* Edges are clipped cell by cell: a box may extend off screen. */
    for (int i = x; i <= x1; i++) {
        if (i >= VIDEO_COLS) break;
        uint8_t c = (i == x) ? g[0] : (i == x1) ? g[1] : g[4];
        chars[y * VIDEO_COLS + i] = c;
        attrs[y * VIDEO_COLS + i] = attr;
        if (y1 < VIDEO_ROWS) {
            uint8_t cb = (i == x) ? g[2] : (i == x1) ? g[3] : g[4];
            chars[y1 * VIDEO_COLS + i] = cb;
            attrs[y1 * VIDEO_COLS + i] = attr;
        }
    }
    for (int j = y + 1; j < y1 && j < VIDEO_ROWS; j++) {
        chars[j * VIDEO_COLS + x] = g[5];
        attrs[j * VIDEO_COLS + x] = attr;
        if (x1 < VIDEO_COLS) {
            chars[j * VIDEO_COLS + x1] = g[5];
            attrs[j * VIDEO_COLS + x1] = attr;
        }
    }
}

static void VIDEO_HOT(apply_text)(video_state_t *v, const video_op_t *op) {
    uint8_t flags = (uint8_t)(op->d >> 24);
    uint8_t attr = (uint8_t)(op->d >> 16);
    int len = (int)(op->d & 0xffff);
    int slot = op->c;
    if (slot >= VIDEO_STAGING_SLOTS || op->a >= VIDEO_COLS || op->b >= VIDEO_ROWS) {
        return;
    }
    const uint8_t *src = s_staging[slot];
    uint8_t *chars = layer_chars(v, flags);
    uint8_t *attrs = layer_attrs(v, flags);
    int idx = op->b * VIDEO_COLS + op->a;
    for (int i = 0; i < len && idx < VIDEO_COLS * VIDEO_ROWS; i++, idx++) {
        if (flags & VIDEO_BLK_BYTES_ATTR) {
            attrs[idx] = src[i];
        } else {
            chars[idx] = src[i];
            if (flags & VIDEO_BLK_SET_ATTR) attrs[idx] = attr;
        }
    }
}

/* Apply one op. Returns true when the palette changed. */
static bool VIDEO_HOT(apply_op)(video_state_t *v, const video_op_t *op) {
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
            s_base_out_count++;
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
            s_overlay_out_count++;
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
        case VIDEO_OP_RECT:
            if (v->mode != VIDEO_MODE_PIXEL) apply_rect(v, op);
            return false;
        case VIDEO_OP_COPY:
            if (v->mode != VIDEO_MODE_PIXEL) apply_copy(v, op);
            return false;
        case VIDEO_OP_SCROLL:
            if (v->mode != VIDEO_MODE_PIXEL) apply_scroll(v, op);
            return false;
        case VIDEO_OP_BOX:
            if (v->mode != VIDEO_MODE_PIXEL) apply_box(v, op);
            return false;
        case VIDEO_OP_TEXT:
            if (v->mode != VIDEO_MODE_PIXEL) apply_text(v, op);
            return false;
        default:
            return false; /* VIDEO_OP_SLOT is handled by the drain */
    }
}

bool VIDEO_HOT(video_ops_drain)(void) {
    s_drain_count++;
    bool palette_changed = false;
    uint32_t tail = s_tail;

    while (s_head != tail) {
        /* Copy the op, apply it, then publish the head: the producer
         * may overwrite the queue slot (and a staging slot the op
         * referenced) as soon as it sees the advance. */
        video_op_t op = s_queue[s_head % VIDEO_QUEUE_OPS];
        if (op.op == VIDEO_OP_SLOT) {
            if (op.a < VIDEO_SLOTS) {
                s_screen_active = op.a;
            }
        } else if (apply_op(&s_screens[s_screen_active], &op)) {
            palette_changed = true;
        }
        atomic_signal_fence(memory_order_seq_cst);
        s_head++;
    }
    return palette_changed;
}

/* video.c — see video.h */
#include "video.h"

#include <stdatomic.h>
#include <stddef.h>

#include "font8x8_rom.h"

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

/* One pixel buffer, shared by whichever slot holds mode 10 (all of it)
 * or mode 11 (its first 160x120 bytes). Only one program can use a
 * pixel mode at a time (Launch from a pixel-mode program is refused), so
 * two slots never need it at once. Core 0 owns it, like the slots. */
static uint8_t s_pixel_buffer[VIDEO_FB_COLS * VIDEO_FB_ROWS];

uint8_t video_font[256][8];

void video_font_init(void) {
    const uint8_t *src = &font8x8_rom[0][0];
    uint8_t *dst = &video_font[0][0];
    for (int i = 0; i < 256 * 8; i++) {
        dst[i] = src[i];
    }
}

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

/* VIDEO_OP_TEXT staging ring (see video.h). Positions are free-running
 * byte counts: the producer hands out bytes up to s_staging_tail, the
 * consumer has applied every op whose bytes end at or before
 * s_staging_released. Ops are applied in queue order, so the released
 * end only moves forward. */
#define STAGING_MASK (VIDEO_STAGING_BYTES - 1u)
static uint8_t s_staging[VIDEO_STAGING_BYTES];
static uint32_t s_staging_tail;              /* producer: core 1 */
static volatile uint32_t s_staging_released; /* consumer: core 0 */

uint8_t *video_staging_acquire(uint32_t len, uint32_t *end) {
    uint32_t start = s_staging_tail;
    uint32_t offset = start & STAGING_MASK;
    if (offset + len > VIDEO_STAGING_BYTES) {
        /* Keep the bytes contiguous: skip the rest of the ring. */
        start += VIDEO_STAGING_BYTES - offset;
        offset = 0;
    }
    uint32_t stop = start + len;
    /* Wait only while unapplied bytes would be overwritten. */
    while ((uint32_t)(stop - s_staging_released) > VIDEO_STAGING_BYTES) {
        video_queue_full_hook();
        atomic_signal_fence(memory_order_seq_cst);
    }
    s_staging_tail = stop;
    *end = stop;
    return &s_staging[offset];
}

/* memset for the core 0 paths: the library's lives in flash. Word
 * stores for the aligned middle (a full mode-4 band is 51 KB); the
 * volatile stores keep the compiler from turning the loops back into a
 * memset call. */
static void VIDEO_HOT(fill)(void *dst, uint8_t value, uint32_t len) {
    volatile uint8_t *p = dst;
    while (len && ((uintptr_t)p & 3u)) {
        *p++ = value;
        len--;
    }
    volatile uint32_t *w = (volatile uint32_t *)p;
    uint32_t word = value * 0x01010101u;
    while (len >= 4) {
        *w++ = word;
        len -= 4;
    }
    p = (volatile uint8_t *)w;
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
    /* 16-231: a 6x6x6 cube at 0, 51, ..., 255 per channel; 232-255: a
     * grey ramp 8, 18, ..., 238 (the xterm 256-colour layout). */
    for (int i = 0; i < 216; i++) {
        uint32_t r = (uint32_t)(i / 36) * 51u;
        uint32_t g = (uint32_t)(i / 6 % 6) * 51u;
        uint32_t b = (uint32_t)(i % 6) * 51u;
        v->palette[16 + i] = (r << 16) | (g << 8) | b;
    }
    for (int i = 0; i < 24; i++) {
        uint32_t grey = 8u + 10u * (uint32_t)i;
        v->palette[232 + i] = grey * 0x010101u;
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
    video_font_init();
    for (int i = 0; i < VIDEO_SLOTS; i++) {
        video_state_init(&s_screens[i]);
    }
    s_screen_active = 0;
    s_head = 0;
    s_tail = 0;
    s_staging_tail = 0;
    s_staging_released = 0;
    s_drain_count = 0;
    s_base_out_count = 0;
    s_overlay_out_count = 0;
    s_lua_mode = VIDEO_MODE_TEXT40;
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
           mode == VIDEO_MODE_TEXT80 || mode == VIDEO_MODE_TEXT80C ||
           mode == VIDEO_MODE_PIXEL || mode == VIDEO_MODE_PIXEL_LO;
}

/* Clear the maps and the frame geometry for a new mode; entering the
 * pixel mode attaches and clears the shared pixel buffer. */
static void VIDEO_HOT(apply_mode)(video_state_t *v, int mode) {
    fill(v->base_char, ' ', sizeof(v->base_char));
    fill(v->base_attr, 0, sizeof(v->base_attr));
    fill(v->overlay_char, 0, sizeof(v->overlay_char));
    fill(v->overlay_attr, VIDEO_ATTR_TRANSPARENT, sizeof(v->overlay_attr));
    if (video_mode_has_pixels(mode)) {
        fill(s_pixel_buffer, 0,
             (uint32_t)(video_pixel_width(mode) * video_pixel_height(mode)));
        v->framebuf = s_pixel_buffer;
    } else {
        v->framebuf = NULL;
    }
    v->mode = (uint8_t)mode;
}

static void VIDEO_HOT(apply_clear)(video_state_t *v, int ch) {
    if (video_mode_has_pixels(v->mode)) {
        if (v->framebuf) {
            fill(v->framebuf, (uint8_t)ch,
                 (uint32_t)(video_pixel_width(v->mode) * video_pixel_height(v->mode)));
        }
        return;
    }
    fill(v->base_char, (uint8_t)ch, sizeof(v->base_char));
    fill(v->base_attr, 0, sizeof(v->base_attr));
}

/* Hide the whole overlay again: blank chars, every cell transparent. */
static void VIDEO_HOT(apply_over_clear)(video_state_t *v, int ch) {
    if (video_mode_has_pixels(v->mode)) {
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
static bool VIDEO_HOT(clip_rect)(const video_state_t *v, int *x, int *y,
                                 int *w, int *h) {
    int cols = video_mode_cols(v->mode), rows = video_mode_rows(v->mode);
    int x1 = *x + *w, y1 = *y + *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (x1 > cols) x1 = cols;
    if (y1 > rows) y1 = rows;
    *w = x1 - *x;
    *h = y1 - *y;
    return *w > 0 && *h > 0;
}

static void VIDEO_HOT(rect_fill)(video_state_t *v, uint8_t flags, int x, int y,
                                 int w, int h, uint8_t ch, uint8_t attr) {
    if (!clip_rect(v, &x, &y, &w, &h)) {
        return;
    }
    int cols = video_mode_cols(v->mode);
    uint8_t *chars = layer_chars(v, flags);
    uint8_t *attrs = layer_attrs(v, flags);
    for (int j = y; j < y + h; j++) {
        int idx = j * cols + x;
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
    int cols = video_mode_cols(v->mode);
    uint8_t *chars = layer_chars(v, flags);
    uint8_t *attrs = layer_attrs(v, flags);
    uint8_t row_c[VIDEO_MAX_COLS], row_a[VIDEO_MAX_COLS];
    bool backwards = dy > sy;
    for (int i = 0; i < h; i++) {
        int r = backwards ? h - 1 - i : i;
        int src = (sy + r) * cols + sx;
        int dst = (dy + r) * cols + dx;
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
    int cols = video_mode_cols(v->mode), rows = video_mode_rows(v->mode);
    /* Clip so both the source and the destination are on screen. */
    if (sx + w > cols) w = cols - sx;
    if (dx + w > cols) w = cols - dx;
    if (sy + h > rows) h = rows - sy;
    if (dy + h > rows) h = rows - dy;
    if (w <= 0 || h <= 0 || sx >= cols || sy >= rows || dx >= cols ||
        dy >= rows) {
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
                chars[j * cols + i] = ch;
                attrs[j * cols + i] = attr;
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
    if (!clip_rect(v, &x, &y, &w, &h)) {
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
    int cols = video_mode_cols(v->mode), rows = video_mode_rows(v->mode);
    if (w < 2 || h < 2 || x >= cols || y >= rows) {
        return;
    }
    const uint8_t *g = s_box_glyphs[style == 2 ? 1 : 0];
    uint8_t *chars = layer_chars(v, flags);
    uint8_t *attrs = layer_attrs(v, flags);
    int x1 = x + w - 1, y1 = y + h - 1;
    /* Edges are clipped cell by cell: a box may extend off screen. */
    for (int i = x; i <= x1; i++) {
        if (i >= cols) break;
        uint8_t c = (i == x) ? g[0] : (i == x1) ? g[1] : g[4];
        chars[y * cols + i] = c;
        attrs[y * cols + i] = attr;
        if (y1 < rows) {
            uint8_t cb = (i == x) ? g[2] : (i == x1) ? g[3] : g[4];
            chars[y1 * cols + i] = cb;
            attrs[y1 * cols + i] = attr;
        }
    }
    for (int j = y + 1; j < y1 && j < rows; j++) {
        chars[j * cols + x] = g[5];
        attrs[j * cols + x] = attr;
        if (x1 < cols) {
            chars[j * cols + x1] = g[5];
            attrs[j * cols + x1] = attr;
        }
    }
}

static void VIDEO_HOT(apply_text)(video_state_t *v, const video_op_t *op) {
    uint8_t flags = (uint8_t)(op->d >> 24);
    uint8_t attr = (uint8_t)(op->d >> 16);
    int len = (int)(op->d & 0xffff);
    uint32_t offset = (op->e - (uint32_t)len) & STAGING_MASK;
    int cols = video_mode_cols(v->mode), rows = video_mode_rows(v->mode);
    if (offset + (uint32_t)len > VIDEO_STAGING_BYTES || op->a >= cols ||
        op->b >= rows) {
        return;
    }
    const uint8_t *src = &s_staging[offset];
    uint8_t *chars = layer_chars(v, flags);
    uint8_t *attrs = layer_attrs(v, flags);
    int idx = op->b * cols + op->a;
    int cells = cols * rows;
    for (int i = 0; i < len && idx < cells; i++, idx++) {
        if (flags & VIDEO_BLK_BYTES_ATTR) {
            attrs[idx] = src[i];
        } else {
            chars[idx] = src[i];
            if (flags & VIDEO_BLK_SET_ATTR) attrs[idx] = attr;
        }
    }
}

/* ---------------- pixel ops (modes 4 and 10) ---------------- */

static inline int VIDEO_HOT(lo16)(uint32_t v) { return (int16_t)(v & 0xffffu); }
static inline int VIDEO_HOT(hi16)(uint32_t v) { return (int16_t)(v >> 16); }

/* One horizontal span, clipped to the pixel buffer. */
static void VIDEO_HOT(px_span)(video_state_t *v, int x0, int x1, int y,
                               uint8_t colour) {
    int w = video_pixel_width(v->mode), h = video_pixel_height(v->mode);
    if (y < 0 || y >= h) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (x0 > x1) return;
    fill(v->framebuf + y * w + x0, colour, (uint32_t)(x1 - x0 + 1));
}

static inline void VIDEO_HOT(px_put)(video_state_t *v, int x, int y, uint8_t colour) {
    int w = video_pixel_width(v->mode);
    if (x >= 0 && x < w && y >= 0 && y < video_pixel_height(v->mode)) {
        v->framebuf[y * w + x] = colour;
    }
}

static void VIDEO_HOT(apply_prect)(video_state_t *v, const video_op_t *op) {
    int x = lo16(op->d), y = hi16(op->d), w = lo16(op->e), h = hi16(op->e);
    if (w <= 0 || h <= 0) return;
    uint8_t colour = op->a;
    if (op->b & VIDEO_PX_FILLED) {
        for (int j = y; j < y + h; j++) px_span(v, x, x + w - 1, j, colour);
        return;
    }
    px_span(v, x, x + w - 1, y, colour);
    px_span(v, x, x + w - 1, y + h - 1, colour);
    for (int j = y + 1; j < y + h - 1; j++) {
        px_put(v, x, j, colour);
        px_put(v, x + w - 1, j, colour);
    }
}

/* Bresenham, clipped per pixel (a line is at most a few hundred). */
static void VIDEO_HOT(apply_pline)(video_state_t *v, const video_op_t *op) {
    int x0 = lo16(op->d), y0 = hi16(op->d), x1 = lo16(op->e), y1 = hi16(op->e);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 4096; guard++) {
        px_put(v, x0, y0, op->a);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Midpoint circle; filled draws the spans between each pair of points. */
static void VIDEO_HOT(apply_pcircle)(video_state_t *v, const video_op_t *op) {
    int cx = lo16(op->d), cy = hi16(op->d), r = (int)(op->e & 0xffff);
    if (r > 1024) return;
    bool filled = op->b & VIDEO_PX_FILLED;
    uint8_t colour = op->a;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        if (filled) {
            px_span(v, cx - x, cx + x, cy + y, colour);
            px_span(v, cx - x, cx + x, cy - y, colour);
            px_span(v, cx - y, cx + y, cy + x, colour);
            px_span(v, cx - y, cx + y, cy - x, colour);
        } else {
            px_put(v, cx + x, cy + y, colour);
            px_put(v, cx - x, cy + y, colour);
            px_put(v, cx + x, cy - y, colour);
            px_put(v, cx - x, cy - y, colour);
            px_put(v, cx + y, cy + x, colour);
            px_put(v, cx - y, cy + x, colour);
            px_put(v, cx + y, cy - x, colour);
            px_put(v, cx - y, cy - x, colour);
        }
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

/* Shift a region's pixels by (dx, dy); uncovered pixels get `a`. Rows
 * and columns are walked away from the direction of travel, so the
 * region can overlap itself without a temporary. */
static void VIDEO_HOT(apply_pscroll)(video_state_t *v, const video_op_t *op) {
    int x = lo16(op->d), y = hi16(op->d), w = lo16(op->e), h = hi16(op->e);
    int dx = (int8_t)op->b, dy = (int8_t)op->c;
    int fw = video_pixel_width(v->mode), fh = video_pixel_height(v->mode);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fw) w = fw - x;
    if (y + h > fh) h = fh - y;
    if (w <= 0 || h <= 0) return;
    uint8_t *fb = v->framebuf;
    for (int i = 0; i < h; i++) {
        int j = dy > 0 ? y + h - 1 - i : y + i; /* destination row */
        uint8_t *dst = fb + j * fw;
        int src_row = j - dy;
        bool row_in = src_row >= y && src_row < y + h;
        const uint8_t *src = fb + src_row * fw;
        for (int k = 0; k < w; k++) {
            int c = dx > 0 ? x + w - 1 - k : x + k; /* destination column */
            int src_col = c - dx;
            if (row_in && src_col >= x && src_col < x + w) {
                dst[c] = src[src_col];
            } else {
                dst[c] = op->a;
            }
        }
    }
}

static inline int VIDEO_HOT(le16)(const uint8_t *p) {
    return (int16_t)(p[0] | (p[1] << 8));
}

/* A w x h block of palette bytes from staging, clipped; with
 * VIDEO_PX_KEYED the key colour (`a`) is transparent. */
static void VIDEO_HOT(apply_blit)(video_state_t *v, const video_op_t *op,
                                  const uint8_t *staged, uint32_t len) {
    if (len < 8) return;
    int x = le16(staged), y = le16(staged + 2);
    int w = le16(staged + 4), h = le16(staged + 6);
    if (w <= 0 || h <= 0 || (uint32_t)(w * h) + 8 > len) return;
    const uint8_t *px = staged + 8;
    int fw = video_pixel_width(v->mode), fh = video_pixel_height(v->mode);
    bool keyed = op->b & VIDEO_PX_KEYED;
    uint8_t key = op->a;
    for (int j = 0; j < h; j++) {
        int ty = y + j;
        if (ty < 0 || ty >= fh) continue;
        uint8_t *dst = v->framebuf + ty * fw;
        const uint8_t *src = px + j * w;
        int i0 = x < 0 ? -x : 0;
        int i1 = x + w > fw ? fw - x : w;
        for (int i = i0; i < i1; i++) {
            uint8_t c = src[i];
            if (!keyed || c != key) dst[x + i] = c;
        }
    }
}

/* Text in the pixel buffer: 8x8 glyphs (the ROM font, or a program's
 * tiles) in colour `a`, over the background `c` when VIDEO_PX_FILLED,
 * each glyph pixel drawn scale x scale (1-4). */
static void VIDEO_HOT(apply_ptext)(video_state_t *v, const video_op_t *op,
                                   const uint8_t *staged, uint32_t len) {
    if (len < 4) return;
    int x = le16(staged), y = le16(staged + 2);
    const uint8_t *text = staged + 4;
    len -= 4;
    bool filled = op->b & VIDEO_PX_FILLED;
    int scale = ((op->b & VIDEO_PX_SCALE_MASK) >> VIDEO_PX_SCALE_SHIFT) + 1;
    int cell = 8 * scale;
    int fw = video_pixel_width(v->mode);
    for (uint32_t n = 0; n < len; n++, x += cell) {
        if (x >= fw) break;
        if (x <= -cell) continue;
        uint8_t ch = text[n];
        const uint8_t *rows = v->tile_defined[ch] ? v->tiles[ch] : video_font[ch];
        for (int r = 0; r < 8; r++) {
            uint8_t bits = rows[r];
            for (int b = 0; b < 8; b++) {
                bool on = bits & (1 << b);
                if (!on && !filled) continue;
                uint8_t colour = on ? op->a : op->c;
                if (scale == 1) {
                    px_put(v, x + b, y + r, colour);
                } else {
                    for (int sy = 0; sy < scale; sy++) {
                        px_span(v, x + b * scale, x + b * scale + scale - 1,
                                y + r * scale + sy, colour);
                    }
                }
            }
        }
    }
}

/* A single-cell op's (a, b) is a cell of the state's text mode. */
static inline bool VIDEO_HOT(cell_in_range)(const video_state_t *v,
                                            const video_op_t *op) {
    return video_mode_has_text(v->mode) && op->a < video_mode_cols(v->mode) &&
           op->b < video_mode_rows(v->mode);
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
            if (cell_in_range(v, op)) {
                int idx = op->b * video_mode_cols(v->mode) + op->a;
                v->base_char[idx] = op->c;
                v->base_attr[idx] = (uint8_t)op->d;
            }
            return false;
        case VIDEO_OP_ATTR:
            if (cell_in_range(v, op)) {
                v->base_attr[op->b * video_mode_cols(v->mode) + op->a] = (uint8_t)op->d;
            }
            return false;
        case VIDEO_OP_CLEAR:
            apply_clear(v, op->a);
            return false;
        case VIDEO_OP_OVER_OUT:
            s_overlay_out_count++;
            if (cell_in_range(v, op)) {
                int idx = op->b * video_mode_cols(v->mode) + op->a;
                v->overlay_char[idx] = op->c;
                v->overlay_attr[idx] = (uint8_t)op->d;
            }
            return false;
        case VIDEO_OP_OVER_ATTR:
            if (cell_in_range(v, op)) {
                v->overlay_attr[op->b * video_mode_cols(v->mode) + op->a] = (uint8_t)op->d;
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
            if (video_mode_has_pixels(v->mode) && v->framebuf) {
                px_put(v, lo16(op->d), hi16(op->d), op->a);
            }
            return false;
        case VIDEO_OP_PRECT:
            if (video_mode_has_pixels(v->mode) && v->framebuf) apply_prect(v, op);
            return false;
        case VIDEO_OP_PLINE:
            if (video_mode_has_pixels(v->mode) && v->framebuf) apply_pline(v, op);
            return false;
        case VIDEO_OP_PCIRCLE:
            if (video_mode_has_pixels(v->mode) && v->framebuf) apply_pcircle(v, op);
            return false;
        case VIDEO_OP_PSCROLL:
            if (video_mode_has_pixels(v->mode) && v->framebuf) apply_pscroll(v, op);
            return false;
        case VIDEO_OP_BLIT:
        case VIDEO_OP_PTEXT: {
            uint32_t len = op->d;
            uint32_t offset = (op->e - len) & STAGING_MASK;
            if (len <= VIDEO_STAGING_BYTES && offset + len <= VIDEO_STAGING_BYTES &&
                video_mode_has_pixels(v->mode) && v->framebuf) {
                if (op->op == VIDEO_OP_BLIT) {
                    apply_blit(v, op, &s_staging[offset], len);
                } else {
                    apply_ptext(v, op, &s_staging[offset], len);
                }
            }
            /* Consumed either way: hand the bytes back to the producer. */
            atomic_signal_fence(memory_order_seq_cst);
            s_staging_released = op->e;
            return false;
        }
        case VIDEO_OP_RECT:
            if (video_mode_has_text(v->mode)) apply_rect(v, op);
            return false;
        case VIDEO_OP_COPY:
            if (video_mode_has_text(v->mode)) apply_copy(v, op);
            return false;
        case VIDEO_OP_SCROLL:
            if (video_mode_has_text(v->mode)) apply_scroll(v, op);
            return false;
        case VIDEO_OP_BOX:
            if (video_mode_has_text(v->mode)) apply_box(v, op);
            return false;
        case VIDEO_OP_TEXT:
            if (video_mode_has_text(v->mode)) apply_text(v, op);
            /* The bytes are consumed: hand them back to the producer. */
            atomic_signal_fence(memory_order_seq_cst);
            s_staging_released = op->e;
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
         * may overwrite the queue slot as soon as it sees the advance
         * (a TEXT op releases its staging bytes inside apply_op). */
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

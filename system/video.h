/* video.h
 *
 * Display state and the core 1 -> core 0 change queue (see AGENTS.md,
 * "Video Subsystem").
 *
 * The Lua Screen* API (screen_lua.c) never touches display memory: it
 * appends small ops to the single-producer/single-consumer queue below.
 * Core 0 drains the queue once per frame boundary, applies the ops to
 * the screen slot the foreground program owns and renders the next
 * frame from that slot. Nothing else is shared between the cores, so
 * there are no frame snapshots and no cross-core copies of the screen.
 *
 * Output is always 640x480. The 2x modes are logically 320x240: 40x30
 * tiles (modes 0/1) or the 320x240 pixel buffer (mode 10), every row
 * and column doubled by the scanout. The 1x modes 2/3 are 80x60 tiles
 * at the full 640x480, one output line per tile row, so a text row
 * costs twice the cells and every row is rendered rather than every
 * second one (see render332.c for the budget).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Tile geometry of the 2x (40x30) and 1x (80x60) text modes. The cell
 * maps are sized for the larger; a state's stride is the cols of its
 * mode (video_mode_cols), never a constant. */
#define VIDEO_COLS_2X 40
#define VIDEO_ROWS_2X 30
#define VIDEO_COLS_1X 80
#define VIDEO_ROWS_1X 60
#define VIDEO_MAX_COLS 80
#define VIDEO_MAX_ROWS 60
#define VIDEO_MAX_CELLS (VIDEO_MAX_COLS * VIDEO_MAX_ROWS)
#define VIDEO_FB_COLS 320
#define VIDEO_FB_ROWS 240

#define VIDEO_MODE_TEXT40 0  /* 40x30, B&W */
#define VIDEO_MODE_TEXT40C 1 /* 40x30, per-cell invert + colour */
#define VIDEO_MODE_TEXT80 2  /* 80x60, B&W */
#define VIDEO_MODE_TEXT80C 3 /* 80x60, per-cell invert + colour */
#define VIDEO_MODE_PIXEL 10  /* 320x240 direct pixels, 256-entry palette */

#define VIDEO_ATTR_TRANSPARENT 0x40

/* One screen slot per program stack level (must cover PROGRAM_MAX). */
#define VIDEO_SLOTS 4

typedef struct {
    uint8_t mode; /* VIDEO_MODE_* */

    /* Base layer: one cell per entry, row-major with the mode's column
     * count as the stride (40 or 80). Attribute byte: bit 7 invert;
     * bits 0-2 colour index. Colour index c uses palette entry c+1 (0 is
     * the background). */
    uint8_t base_char[VIDEO_MAX_CELLS];
    uint8_t base_attr[VIDEO_MAX_CELLS];

    /* One overlay layer, composited over the base. A cell is transparent
     * while its attribute has bit 6 set: OverlayOut/OverlayAttr clear
     * the bit for the cells they touch and OverlayClear sets it
     * everywhere. */
    uint8_t overlay_char[VIDEO_MAX_CELLS];
    uint8_t overlay_attr[VIDEO_MAX_CELLS];

    /* RAM tile override set; ROM font (font8x8, ASCII-aligned) used
     * where tile_defined[i] == 0. Each tile is 8 row bytes; bit 0 of a
     * row byte is the leftmost pixel (same as the ROM font). */
    uint8_t tiles[256][8];
    uint8_t tile_defined[256];

    /* Mode 10 only: 320x240 palette indexes into core 0's shared pixel
     * buffer, attached when the mode is entered. */
    uint8_t *framebuf;

    uint32_t palette[256]; /* RGB888 */
} video_state_t;

/* ------------------------------------------------------------------ */
/* Change queue (producer = core 1, consumer = core 0)                 */
/* ------------------------------------------------------------------ */

typedef enum {
    VIDEO_OP_RESET,      /* fresh screen: mode 0, maps/tiles/palette reset */
    VIDEO_OP_MODE,       /* a = mode */
    VIDEO_OP_OUT,        /* base: a,b = x,y; c = char; d = attr */
    VIDEO_OP_ATTR,       /* base: a,b = x,y; d = flags */
    VIDEO_OP_CLEAR,      /* base: a = fill char */
    VIDEO_OP_OVER_OUT,   /* overlay: a,b = x,y; c = char; d = attr */
    VIDEO_OP_OVER_ATTR,  /* overlay: a,b = x,y; d = flags */
    VIDEO_OP_OVER_CLEAR, /* overlay: a = fill char */
    VIDEO_OP_TILE,       /* a = index; d,e = the 8 row bytes */
    VIDEO_OP_PALETTE,    /* a = index; d = 0xRRGGBB */
    VIDEO_OP_PLOT,       /* a,b = x,y; d = colour (mode 10) */
    VIDEO_OP_SLOT,       /* a = screen slot for the ops that follow */

    /* Block ops (text modes): one op per rectangle instead of one per
     * cell. Rectangles are clipped to the screen by the producer, so
     * x, y, w, h all fit a byte. `flags` selects the layer and what the
     * op writes (VIDEO_BLK_*). */
    VIDEO_OP_RECT,   /* a,b = x,y; c = w; d = h | ch<<8 | attr<<16 | flags<<24 */
    VIDEO_OP_COPY,   /* a,b = sx,sy; c = w; d = h | dx<<8 | dy<<16 | flags<<24;
                        e = fill ch | fill attr<<8 (VIDEO_BLK_CLEAR: move) */
    VIDEO_OP_SCROLL, /* a,b = x,y; c = w; d = h | (int8)dx<<8 | (int8)dy<<16 |
                        flags<<24; e = fill ch | fill attr<<8 */
    VIDEO_OP_BOX,    /* a,b = x,y; c = w; d = h | style<<8 | attr<<16 | flags<<24 */
    VIDEO_OP_TEXT,   /* a,b = x,y; d = len | attr<<16 | flags<<24; e = the
                        free-running end of its staging bytes: `len` bytes
                        ending there, written from (x,y) onwards, wrapping
                        to the next row */
} video_op_kind_t;

/* Block op flags (the top byte of `d`). */
#define VIDEO_BLK_OVERLAY 0x01 /* overlay layer instead of the base */
#define VIDEO_BLK_CHARS 0x02   /* RECT: write the character */
#define VIDEO_BLK_ATTRS 0x04   /* RECT: write the attribute */
#define VIDEO_BLK_CLEAR 0x02   /* COPY: clear the source (a move) */
#define VIDEO_BLK_BYTES_ATTR 0x02 /* TEXT: the bytes are attributes */
#define VIDEO_BLK_SET_ATTR 0x04   /* TEXT: also write `attr` per cell */

typedef struct {
    uint8_t op;
    uint8_t a, b, c;
    uint32_t d, e;
} video_op_t;

/* Ops between frame boundaries; a full queue blocks the producer until
 * core 0 drains it (bounded by one frame). */
#define VIDEO_QUEUE_OPS 1024

/* Staging for VIDEO_OP_TEXT: the bytes travel outside the 12-byte op,
 * in a byte ring. The producer takes exactly the bytes a write needs,
 * contiguous, fills them and queues the op with `e` = the end it was
 * given; core 0 releases them as it applies the op. Core 0 drains once a
 * frame, so the ring bounds how much text core 1 can queue per frame
 * before it waits: 8 KB is a full 80x60 screen plus a 40x30 one, or
 * hundreds of status-line strings. (Two whole-screen slots used to stall
 * core 1 for a frame at the third write of any frame.) Must be a power
 * of two, and at least VIDEO_MAX_CELLS. */
#define VIDEO_STAGING_BYTES 8192u

/* Append one op (core 1). */
void video_op_put(const video_op_t *op);

/* Take `len` (1..VIDEO_MAX_CELLS) contiguous staging bytes (core
 * 1): returns where to write them and sets *end for the op's `e`. Blocks
 * only while the ring is full of bytes core 0 has not applied yet. Every
 * acquire must be followed by queuing its op. */
uint8_t *video_staging_acquire(uint32_t len, uint32_t *end);

/* Weak hook run while video_op_put waits on a full queue: a no-op on
 * the firmware, a drain in the single-threaded simulator. */
void video_queue_full_hook(void);

/* Weak hook run while WaitVSync spins on the frame counter: a no-op on
 * the firmware (core 0 advances the counter), while the simulator
 * advances it with real time and drains the queue, so a program that
 * waits for frames keeps pace instead of timing out. */
void video_frame_wait_hook(void);

/* Latch a mode change into the core-1-side shadow (video_lua_mode()),
 * used by the process model to know when a pixel-mode program is on
 * top. Also used by the API layer when it queues VIDEO_OP_MODE/RESET. */
void video_note_mode(int mode);

/* ------------------------------------------------------------------ */
/* Core-0 side                                                         */
/* ------------------------------------------------------------------ */

/* Initialise every slot and the queue. Called by core 0 before the
 * scanout starts; host tests and the simulator call it too. */
void video_screens_init(void);

/* Apply every queued op, in order, to the slot in force. Called by core
 * 0 once per frame boundary before the next frame is rendered. Returns
 * true when the palette changed (the caller rebuilds its RGB332 LUT). */
bool video_ops_drain(void);

/* The screen the scanout renders from. */
video_state_t *video_screen(void);
int video_screen_index(void);
uint32_t video_ops_pending(void);
uint32_t video_ops_drain_count(void);
uint32_t video_ops_base_out_count(void);
uint32_t video_ops_overlay_out_count(void);

/* Core-1-side shadow of the mode Lua last asked for. */
int video_lua_mode(void);

/* Reset a state to its power-on contents (mode 0, blank maps, default
 * palette). Host tests use it to build render inputs. */
void video_state_init(video_state_t *v);

/* Mode geometry. Inline so core 0's SRAM-resident render path can use
 * them without a call into flash. */
bool video_mode_valid(int mode);

/* True for the modes the scanout doubles (320x240 logical). */
static inline bool video_mode_2x(int mode) {
    return mode != VIDEO_MODE_TEXT80 && mode != VIDEO_MODE_TEXT80C;
}

/* True for the colour text modes (attribute bits 0-2 select a colour). */
static inline bool video_mode_colour(int mode) {
    return mode == VIDEO_MODE_TEXT40C || mode == VIDEO_MODE_TEXT80C;
}

/* Active cells (text modes) or pixels (mode 10) across and down. */
static inline int video_mode_cols(int mode) {
    if (mode == VIDEO_MODE_PIXEL) return VIDEO_FB_COLS;
    return video_mode_2x(mode) ? VIDEO_COLS_2X : VIDEO_COLS_1X;
}

static inline int video_mode_rows(int mode) {
    if (mode == VIDEO_MODE_PIXEL) return VIDEO_FB_ROWS;
    return video_mode_2x(mode) ? VIDEO_ROWS_2X : VIDEO_ROWS_1X;
}

/* Logical rows the renderer produces per frame: 240 (each shown twice)
 * or 480. */
static inline int video_mode_lines(int mode) {
    return video_mode_2x(mode) ? VIDEO_FB_ROWS : 2 * VIDEO_FB_ROWS;
}

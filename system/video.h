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
 * Logical geometry is fixed at 320x240: 40x30 tiles (modes 0/1) or a
 * 320x240 pixel buffer (mode 10). The scanout doubles every row and
 * column to 640x480; the 80-column modes 2 and 3 are retired.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VIDEO_COLS 40
#define VIDEO_ROWS 30
#define VIDEO_FB_COLS 320
#define VIDEO_FB_ROWS 240

#define VIDEO_MODE_TEXT40 0  /* 40x30, B&W */
#define VIDEO_MODE_TEXT40C 1 /* 40x30, per-cell invert + colour */
#define VIDEO_MODE_PIXEL 10  /* 320x240 direct pixels, 256-entry palette */

#define VIDEO_ATTR_TRANSPARENT 0x40

/* One screen slot per program stack level (must cover PROGRAM_MAX). */
#define VIDEO_SLOTS 4

typedef struct {
    uint8_t mode; /* VIDEO_MODE_* */

    /* Base layer: one 40x30 cell per entry. Attribute byte: bit 7
     * invert; bits 0-2 colour index. Colour index c uses palette entry
     * c+1 (0 is the background). */
    uint8_t base_char[VIDEO_COLS * VIDEO_ROWS];
    uint8_t base_attr[VIDEO_COLS * VIDEO_ROWS];

    /* One overlay layer, composited over the base. A cell is transparent
     * while its attribute has bit 6 set: OverlayOut/OverlayAttr clear
     * the bit for the cells they touch and OverlayClear sets it
     * everywhere. */
    uint8_t overlay_char[VIDEO_COLS * VIDEO_ROWS];
    uint8_t overlay_attr[VIDEO_COLS * VIDEO_ROWS];

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
} video_op_kind_t;

typedef struct {
    uint8_t op;
    uint8_t a, b, c;
    uint32_t d, e;
} video_op_t;

/* Ops between frame boundaries; a full queue blocks the producer until
 * core 0 drains it (bounded by one frame). */
#define VIDEO_QUEUE_OPS 1024

/* Append one op (core 1). */
void video_op_put(const video_op_t *op);

/* Weak hook run while video_op_put waits on a full queue: a no-op on
 * the firmware, a drain in the single-threaded simulator. */
void video_queue_full_hook(void);

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

/* Active dimensions for a mode; false when the mode is unsupported. */
int video_mode_cols(int mode);
int video_mode_rows(int mode);
bool video_mode_valid(int mode);

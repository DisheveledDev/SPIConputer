/* scanout.h
 *
 * HSTX scanline sequencer (Phase 7). Platform-neutral: all DMA register
 * access goes through scanout_hw_post(), which the product board
 * implements in core0/video_hw.c and the host tests implement against a
 * memory trace. Everything else (the ping/pong cadence, the vertical
 * timing state machine and the line ring bookkeeping) is plain C, so
 * the frame structure is unit-testable without hardware.
 *
 * Timing is fixed 640x480@60 with negative sync polarity, matching the
 * TMDS symbol constants in the hardware layer.
 *
 * Ring protocol (producer = the render context, consumer = the DMA IRQ).
 * The ring holds logical rows of the *current frame*: row r lives in
 * slot r % SCANOUT_RING_LINES.
 *
 *   - rows_published counts the rows the producer has finished this
 *     frame (rows 0..rows_published-1 hold valid data); rows_consumed
 *     counts the rows whose last output line has been posted. Both are
 *     0 at the start of each active region, so rows_consumed never
 *     exceeds rows_published;
 *   - the consumer posts slot r % RING for row r while
 *     r < rows_published; otherwise it re-shows the last published row
 *     and counts an underrun (before the first frame the ring is zeroed,
 *     which shows black);
 *   - the producer may render row r while
 *     r - rows_consumed < SCANOUT_RING_LINES - 2. The pipeline costs two
 *     slots: a buffer posted at completion IRQ S is transferred between
 *     IRQ S+1 and IRQ S+2 (the other channel's command list runs first),
 *     and the buffer posted at S-1 is being transferred while the
 *     producer runs, so the two most recently posted rows are still in
 *     flight and must not be overwritten;
 *   - when the last row's last line is posted the active region ends:
 *     both counters reset and scanout_frame_begin() refreshes the frame
 *     geometry, so the vblank prefetch renders the *next* frame with the
 *     right mode and cannot run away (it stops after
 *     SCANOUT_RING_LINES - 2 rows).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SCANOUT_H_TOTAL_PIXELS 800
#define SCANOUT_H_ACTIVE_PIXELS 640

/* Vertical timing. The pixel clock is fixed at 25.2 MHz (the HSTX clock
 * is clk_hstx / 5), so the refresh rate follows from the total line
 * count: 800 x 525 = 59.94 Hz, 800 x 630 = exactly 50.0 Hz. The default
 * is 60 Hz, the standard VESA 640x480@60 timing every DVI/HDMI sink must
 * accept; the 50 Hz variant (a 4.8 ms vblank for core 0 instead of
 * 1.4 ms, 17% less line rendering) is a non-standard mode that some
 * monitors refuse, so it is opt-in: -DSPICOMPUTER_REFRESH_HZ=50. */
#ifndef SPICOMPUTER_REFRESH_HZ
#define SPICOMPUTER_REFRESH_HZ 60
#endif

#if SPICOMPUTER_REFRESH_HZ == 50
#define SCANOUT_V_FRONT_PORCH 10
#define SCANOUT_V_SYNC_WIDTH 2
#define SCANOUT_V_BACK_PORCH 138
#elif SPICOMPUTER_REFRESH_HZ == 60
#define SCANOUT_V_FRONT_PORCH 10
#define SCANOUT_V_SYNC_WIDTH 2
#define SCANOUT_V_BACK_PORCH 33
#else
#error "SPICOMPUTER_REFRESH_HZ must be 50 or 60"
#endif

#define SCANOUT_V_ACTIVE_LINES 480
#define SCANOUT_V_BLANK_LINES \
    (SCANOUT_V_FRONT_PORCH + SCANOUT_V_SYNC_WIDTH + SCANOUT_V_BACK_PORCH)
#define SCANOUT_V_TOTAL_LINES \
    (SCANOUT_V_BLANK_LINES + SCANOUT_V_ACTIVE_LINES)

#define SCANOUT_WORDS_PER_LINE (SCANOUT_H_ACTIVE_PIXELS / 4)

/* Ring depth must stay a power of two. */
#define SCANOUT_RING_LINES 8

/* Use every ring row except the two that can still be in the HSTX DMA
 * pipeline. The extra three rows materially increase tolerance of a slow
 * text renderer without allowing the producer to overwrite in-flight DMA. */
#define SCANOUT_RING_AHEAD (SCANOUT_RING_LINES - 2)

typedef struct {
    const uint32_t *vblank_on;  /* 7 words, set once by the driver */
    const uint32_t *vblank_off; /* 7 words, set once by the driver */
    const uint32_t *cmdlist;    /* 9 words, set once by the driver */
    uint32_t *ring[SCANOUT_RING_LINES];

    uint32_t scanline; /* current output line, 0..524 */

    /* Frame parameters, refreshed by scanout_frame_begin() when an
     * active region ends (before the vblank prefetch). */
    uint32_t rows_total; /* logical rows this frame (240 or 480) */
    bool row_2x;         /* 2x modes: every row is two output lines */
    bool frame_active;   /* inside this frame's active region */

    /* Producer (render context): rows 0..rows_published-1 are valid. */
    uint32_t rows_published;

    /* Consumer (DMA IRQ): rows whose last output line has been posted. */
    uint32_t rows_consumed;

    uint32_t underruns;   /* consumer showed a stale line */
    uint32_t freeze_slot; /* ring slot of the last published row */
    uint32_t posted_row;  /* row of the most recent pixel post */
    uint32_t frame;       /* completed frames */
    bool post_cmdlist;    /* next active-line step is the cmdlist */
    bool channel_pong;    /* which DMA channel just finished */
} scanout_t;

/* Hardware step: post up to `words` 32-bit words from `src` on
 * `channel` (0 = ping, 1 = pong), then clear that channel's interrupt
 * flag. Implemented by the product board (writes the register block)
 * and by the host tests (records the trace). */
void scanout_hw_post(unsigned channel, const uint32_t *src, uint32_t words);

void scanout_init(scanout_t *s);

/* Called from scanout_step() when an active region ends, and by the
 * driver at init: refreshes rows_total / row_2x from the current video
 * mode. */
void scanout_frame_begin(scanout_t *s);

/* One DMA-completion IRQ: post the next transfer and advance state. */
void scanout_step(scanout_t *s);

/* Producer side (render context only): true when row `rows_published`
 * may be rendered into its slot without touching a row that is in the
 * DMA pipeline. */
static inline bool scanout_ring_can_publish(const scanout_t *s) {
    return s->rows_published < s->rows_total &&
           s->rows_published - s->rows_consumed < SCANOUT_RING_AHEAD;
}

/* Producer side: the slot for the next row to render. */
static inline uint32_t *scanout_ring_next(scanout_t *s) {
    return s->ring[s->rows_published % SCANOUT_RING_LINES];
}

/* Producer side: publish the row just rendered into scanout_ring_next().
 * Returns false (and publishes nothing) if a frame boundary passed while
 * rendering, so the caller re-renders for the new frame instead of
 * publishing a row of the wrong frame. */
static inline bool scanout_ring_publish(scanout_t *s, uint32_t row) {
    if (s->rows_published != row) {
        return false;
    }
    s->rows_published = row + 1u;
    return true;
}

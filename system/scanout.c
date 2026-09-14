/* scanout.c — HSTX scanline sequencer; see scanout.h */
#include "scanout.h"

/* The sequencer runs from the DMA completion IRQ on every scanline, so
 * on the product board it lives in SRAM: an XIP cache miss there would
 * push the next post past the blanking interval. */
#if defined(PICO_RP2040) || defined(PICO_RP2350)
#include "pico.h"
#define SCANOUT_HOT(name) __not_in_flash_func(name)
#else
#define SCANOUT_HOT(name) name
#endif

void scanout_init(scanout_t *s) {
    s->scanline = 2;
    s->rows_total = SCANOUT_V_ACTIVE_LINES;
    s->row_2x = false;
    s->frame_active = false;
    s->rows_published = 0;
    s->rows_consumed = 0;
    s->underruns = 0;
    s->freeze_slot = 0;
    s->posted_row = 0;
    s->frame = 0;
    s->post_cmdlist = false;
    s->channel_pong = false;
}

/* Logical row shown on the active output line `line_index` (0..479). */
static inline uint32_t row_for_line(uint32_t line_index, bool row_2x) {
    return row_2x ? line_index / 2u : line_index;
}

void SCANOUT_HOT(scanout_step)(scanout_t *s) {
    unsigned ch = s->channel_pong ? 1u : 0u; /* just-finished channel */
    s->channel_pong = !s->channel_pong;

    uint32_t line = s->scanline;

    if (line >= SCANOUT_V_FRONT_PORCH &&
        line < SCANOUT_V_FRONT_PORCH + SCANOUT_V_SYNC_WIDTH) {
        scanout_hw_post(ch, s->vblank_on, 7);
        s->scanline = (line + 1) % SCANOUT_V_TOTAL_LINES;
        return;
    }
    if (line < SCANOUT_V_FRONT_PORCH + SCANOUT_V_SYNC_WIDTH +
                   SCANOUT_V_BACK_PORCH) {
        scanout_hw_post(ch, s->vblank_off, 7);
        s->scanline = (line + 1) % SCANOUT_V_TOTAL_LINES;
        return;
    }

    if (!s->post_cmdlist) {
        /* First IRQ of a new active line: park the channel on the
         * horizontal porch/sync command list (no pixel data yet). */
        scanout_hw_post(ch, s->cmdlist, 9);
        s->post_cmdlist = true;
        return;
    }

    /* Second IRQ of the active line: pixel data. The ring holds logical
     * rows; in 2x modes each row is shown on two consecutive output
     * lines, so `rows_consumed` only advances when a row's second line
     * has been posted. */
    if (line == SCANOUT_V_FRONT_PORCH + SCANOUT_V_SYNC_WIDTH +
                    SCANOUT_V_BACK_PORCH) {
        /* First active line of a frame. Everything was rebased at the
         * end of the previous active region, so the ring holds the rows
         * the producer prefetched during vblank. */
        s->frame_active = true;
        s->frame++;
    }

    uint32_t line_index =
        line - (SCANOUT_V_TOTAL_LINES - SCANOUT_V_ACTIVE_LINES);
    uint32_t row = row_for_line(line_index, s->row_2x);
    uint32_t slot;
    if (row < s->rows_published) {
        slot = row;
        s->freeze_slot = slot;
    } else {
        /* The producer has not published this row yet: freeze the
         * picture on the last row it did publish (the zeroed ring before
         * the first frame, which is black). */
        slot = s->freeze_slot;
        s->underruns++;
    }
    s->posted_row = slot;

    scanout_hw_post(ch, s->ring[slot % SCANOUT_RING_LINES],
                    SCANOUT_WORDS_PER_LINE);

    s->scanline = (line + 1) % SCANOUT_V_TOTAL_LINES;
    s->post_cmdlist = false;

    /* A row is fully posted on its last output line (its only line in
     * 1x modes, the second line in 2x modes). */
    if (row < s->rows_published &&
        line_index == (s->row_2x ? row * 2u + 1u : row)) {
        s->rows_consumed = row + 1u;
    }

    /* The active region ends when the last row's last line is posted.
     * Rebase the ring: the counters return to 0 and the frame geometry
     * is refreshed, so the vblank prefetch renders the *next* frame with
     * the right mode and is bounded by rows_total. */
    if (s->rows_consumed >= s->rows_total) {
        s->frame_active = false;
        s->rows_published = 0;
        s->rows_consumed = 0;
        s->freeze_slot = 0;
        scanout_frame_begin(s);
    }
}

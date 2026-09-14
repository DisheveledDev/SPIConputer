/* core0/video_hw.c
 *
 * HDMI output over the RP2350 HSTX peripheral (Phase 7).
 *
 * Design (see improvements.md items 1-9):
 *   - The HSTX TMDS expander is configured for RGB332, as in the
 *     pico-examples dvi_out_hstx_encoder demo: command lists supply the
 *     syncs and blanking, the expander serialises 4 pixels per word.
 *   - The scanline sequencer lives in scanout.c (host-tested); this
 *     file provides the DMA register glue: two claimed channels in a
 *     ping/pong chain, one word per HSTX FIFO entry.
 *   - A render pump (video_hw_poll) draws ahead into a ring of packed
 *     RGB332 lines: the DMA IRQ only posts the next buffer and raises
 *     a flag, and the rendering itself happens in core 0's main loop,
 *     where the IRQ can preempt it. (Rendering in an ISR of its own
 *     starves the 8-word HSTX FIFO while a row is drawn, which drops
 *     the monitor's sync.) The ring gives the renderer (ring depth) x
 *     (line period) of slack instead of the horizontal blanking
 *     interval.
 *   - The DMA IRQ runs on DMA_IRQ_2, clear of the SD driver's
 *     DMA_IRQ_0/1 use (FatFs_SPI/sd_driver/spi.c). The channels are
 *     claimed dynamically, so they cannot collide with the SD driver's
 *     either; video_hw_init() runs before the card is mounted so the
 *     assignment is stable.
 *
 * Pixel clock: the HSTX CSR divides clk_hstx by 5 and shifts 2 bits per
 * cycle, so the pixel clock is clk_hstx / 5. core0/main.c sets clk_sys
 * to 126 MHz, giving 25.2 MHz (the 640x480 timing, 252 Mbps, VESA's
 * 25.175 MHz spec). With the SDK default 150 MHz the pixel clock is
 * 30 MHz and the mode runs ~71 Hz: still locked by most monitors, but
 * off-spec.
 */
#include "video_hw.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"
#include "pico/stdlib.h"

#if defined(PICO_RP2350) && defined(SPICOMPUTER_HAS_HDMI)

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/timer.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "pico/platform.h"

#include "render332.h"
#include "scanout.h"
#include "system_state.h"
#include "video.h"

/* ------------------------------------------------------------------ */
/* HSTX command lists                                                 */
/* ------------------------------------------------------------------ */

#define HSTX_CMD_RAW_REPEAT (1u << 12)
#define HSTX_CMD_TMDS (2u << 12)
#define HSTX_CMD_NOP (0xfu << 12)

#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu
/* Negative sync polarity, 640x480 VESA timing. */
#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

#define MODE_H_FRONT_PORCH 16
#define MODE_H_SYNC_WIDTH 96
#define MODE_H_BACK_PORCH 48

_Static_assert(SCANOUT_H_ACTIVE_PIXELS == RENDER_OUT_WIDTH &&
                   SCANOUT_V_ACTIVE_LINES == RENDER_OUT_HEIGHT,
               "scanout and renderer geometry must match");
_Static_assert(SCANOUT_H_TOTAL_PIXELS * SCANOUT_V_TOTAL_LINES *
                       SPICOMPUTER_REFRESH_HZ ==
                   25200000,
               "vertical timing must give the 25.2 MHz pixel clock");

static const uint32_t s_vblank_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + SCANOUT_H_ACTIVE_PIXELS),
    SYNC_V0_H1,
    HSTX_CMD_NOP,
};

static const uint32_t s_vblank_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + SCANOUT_H_ACTIVE_PIXELS),
    SYNC_V1_H1,
    HSTX_CMD_NOP,
};

static const uint32_t s_vactive_cmdlist[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS | SCANOUT_H_ACTIVE_PIXELS,
};

/* ------------------------------------------------------------------ */
/* Driver state                                                       */
/* ------------------------------------------------------------------ */

static scanout_t s_scanout;
static uint32_t s_ring[SCANOUT_RING_LINES][SCANOUT_WORDS_PER_LINE];

/* Rendering runs in core 0's main loop (video_hw_poll), never in an
 * IRQ: the DMA completion IRQ must be able to preempt it at any point,
 * because a late post starves the 8-word HSTX FIFO (~1.3 us of pixels)
 * and drops the monitor's sync. A row render takes long enough to do
 * exactly that, so the IRQ only raises this flag. */
static volatile bool s_render_pending;

/* A frame boundary passed: collect the ops core 1 queued and apply them
 * to the screen slots before rendering the next frame. Set from the
 * scanout ISR (scanout_frame_begin), handled in video_hw_poll. */
static volatile bool s_drain_pending;

static int s_dma_ping;
static int s_dma_pong;
static bool s_started;

/* Bring-up diagnostics (read by core 1 once per second; see
 * video_hw.h). Counters are cumulative since boot. */
static uint32_t s_diag_fifo_empty;
static uint32_t s_diag_fifo_wofs;
static uint32_t s_diag_gap_max_us;
static uint32_t s_diag_long_gaps;
static uint32_t s_diag_steps;       /* scanout_step calls this frame */
static uint32_t s_diag_skews;       /* frames with a step-count mismatch */
static uint32_t s_diag_last_steps;  /* most recent bad step count */
static uint32_t s_diag_frame_begins;
static uint32_t s_diag_last_irq_us;
static bool s_diag_first_irq = true;

/* Details of the most recent >64 us completion gap (see the IRQ). */
static uint32_t s_diag_evt_gap_us;
static uint32_t s_diag_evt_frame;
static uint32_t s_diag_evt_line;
static uint32_t s_diag_evt_fifo;
static uint32_t s_diag_evt_cmdlist;
static uint32_t s_diag_evt_underruns;

/* One frame is exactly (blanking lines) + 480 lines * (cmdlist, pixels). */
#define SCANOUT_STEPS_PER_FRAME \
    (SCANOUT_V_BLANK_LINES + SCANOUT_V_ACTIVE_LINES * 2u)

/* ------------------------------------------------------------------ */
/* Hardware glue for scanout.c                                        */
/* ------------------------------------------------------------------ */

void __not_in_flash_func(scanout_hw_post)(unsigned channel,
                                          const uint32_t *src,
                                          uint32_t words) {
    uint ch = (channel == 0) ? (uint)s_dma_ping : (uint)s_dma_pong;
    dma_channel_hw_t *dma = &dma_hw->ch[ch];
    dma->read_addr = (uintptr_t)src;
    dma->transfer_count = words;
    dma_hw->ints2 = 1u << ch;
}

static void __not_in_flash_func(hstx_dma_irq)(void) {
    /* Time-critical work first: re-arm the channel that just finished
     * and publish the next buffer, then tell the main loop there may be
     * rows to render. Diagnostics run last so they can only ever add
     * a few cycles after the post. */
    scanout_step(&s_scanout);
    g_system_state.video_frame_count = s_scanout.frame;
    s_render_pending = true;

    /* Diagnostics: how long since the previous completion (a line is
     * ~31.7 us), and whether the FIFO was empty or overflowed. A
     * completion gap over 64 us means the scanout stream stalled: keep
     * the details of the most recent one so the OS core can log exactly
     * where in the frame it happened. */
    uint32_t now = time_us_32();
    if (!s_diag_first_irq) {
        uint32_t gap = now - s_diag_last_irq_us;
        if (gap > s_diag_gap_max_us) {
            s_diag_gap_max_us = gap;
        }
        if (gap > 64u) {
            s_diag_long_gaps++;
            s_diag_evt_gap_us = gap;
            s_diag_evt_frame = s_scanout.frame;
            s_diag_evt_line = s_scanout.scanline;
            s_diag_evt_fifo = hstx_fifo_hw->stat & 0xffu;
            s_diag_evt_cmdlist = s_scanout.post_cmdlist ? 1u : 0u;
            s_diag_evt_underruns = s_scanout.underruns;
        }
    }
    s_diag_first_irq = false;
    s_diag_last_irq_us = now;
    uint32_t fifo_stat = hstx_fifo_hw->stat;
    if (fifo_stat & HSTX_FIFO_STAT_EMPTY_BITS) {
        s_diag_fifo_empty++;
    }
    if (fifo_stat & HSTX_FIFO_STAT_WOF_BITS) {
        s_diag_fifo_wofs++;
        hstx_fifo_hw->stat = HSTX_FIFO_STAT_WOF_BITS; /* write 1 to clear */
    }
    s_diag_steps++;
}

/* ------------------------------------------------------------------ */
/* Rendering (render ISR context)                                     */
/* ------------------------------------------------------------------ */

void __not_in_flash_func(scanout_frame_begin)(scanout_t *s) {
    /* Diagnostics: a frame boundary must fall after exactly
     * SCANOUT_STEPS_PER_FRAME DMA steps. Skip the first call (init runs
     * mid-frame, so its count is not a full frame). */
    if (s_diag_frame_begins++ > 0 && s_diag_steps != SCANOUT_STEPS_PER_FRAME) {
        s_diag_skews++;
        s_diag_last_steps = s_diag_steps;
    }
    s_diag_steps = 0;

#if defined(SPICOMPUTER_VIDEO_TEST_PATTERN)
    /* The test pattern is drawn at the full 640x480 resolution. */
    s->rows_total = RENDER_OUT_HEIGHT;
    s->row_2x = false;
#else
    /* Logical geometry is fixed: 240 rows, every row scanned twice.
     * Core 1's queued ops are collected at this boundary (see
     * video_hw_poll) before the next frame is rendered. */
    s->rows_total = VIDEO_ROWS;
    s->row_2x = true;
#endif
    s_drain_pending = true;
}

#if defined(SPICOMPUTER_VIDEO_TEST_PATTERN)

/* Diagnostic pattern (bring-up builds only): a cheap, static image that
 * exercises the scanout without needing a program, plus two live
 * counters shown as bars in the top band so the bench can report numbers
 * without a console:
 *
 *   red bar    = ring underruns since boot (1 px per underrun)
 *   yellow bar = worst row render time of the last frame (4 px per us;
 *                a 1x line is 31.7 us, i.e. 127 px)
 *
 * Bands, top to bottom (with a 1 px white border):
 *   y   0.. 31  status bars (above)
 *   y  32.. 95  colour bars: black, blue, red, magenta, green, cyan,
 *               yellow, white, dark grey, light grey (64 px each)
 *   y  96..159  1 px black/white vertical stripes (fine detail)
 *   y 160..223  2 px black / 1 px white stripes (pixel order)
 *   y 224..287  horizontal grey ramp (column continuity)
 *   y 288..351  vertical grey ramp, one level per row (row continuity:
 *               flat bands mean repeated/stale rows)
 *   y 352..415  8 px grid on mid grey (geometry)
 *   y 416..479  1 px diagonal + a white bar sweeping left to right
 *               (frames advancing)
 *
 * Row rendering is a constant fill or a 160-word copy: the per-pixel
 * work below happens once, in pattern_init(), so the producer always
 * beats the line rate. That is deliberate - a pattern slow enough to
 * starve the ring would hide the very fault it is meant to find.
 */
enum { PAT_BARS, PAT_STRIPES, PAT_2ON1OFF, PAT_HRAMP, PAT_GRID, PAT_ROWS };

static uint32_t s_pat[PAT_ROWS][SCANOUT_WORDS_PER_LINE];
static uint32_t s_pat_diag[64][SCANOUT_WORDS_PER_LINE];
static bool s_pat_ready;

static uint8_t pattern_grey(uint32_t level) {
    uint8_t v = (uint8_t)level;
    return (uint8_t)((v & 0xe0u) | ((v & 0xe0u) >> 3) | ((v & 0xc0u) >> 6));
}

/* Per-pixel definitions, used only by pattern_init(). */
static uint8_t pattern_px(unsigned band, uint32_t x) {
    static const uint32_t bars[10] = {
        0x000000, 0x0000ff, 0xff0000, 0xff00ff, 0x00ff00,
        0x00ffff, 0xffff00, 0xffffff, 0x404040, 0xc0c0c0,
    };
    switch (band) {
        case PAT_BARS:
            return render332_rgb(bars[(x / 64) % 10]);
        case PAT_STRIPES:
            return (x & 1) ? 0xff : 0x00;
        case PAT_2ON1OFF:
            return (x % 3) < 2 ? 0x00 : 0xff;
        case PAT_HRAMP:
            return pattern_grey((x * 255) / (SCANOUT_H_ACTIVE_PIXELS - 1));
        default: /* PAT_GRID */
            return ((x & 7) == 0) ? pattern_grey(0x20) : pattern_grey(0x80);
    }
}

static void pattern_init(void) {
    for (int band = 0; band < PAT_ROWS; band++) {
        for (uint32_t w = 0; w < SCANOUT_WORDS_PER_LINE; w++) {
            uint32_t word = 0;
            for (uint32_t i = 0; i < 4; i++) {
                word |= (uint32_t)pattern_px((unsigned)band, w * 4 + i)
                        << (i * 8);
            }
            s_pat[band][w] = word;
        }
    }
    for (uint32_t row = 0; row < 64; row++) {
        uint32_t dx = (row * (SCANOUT_H_ACTIVE_PIXELS - 1)) / 63;
        for (uint32_t x = 0; x < SCANOUT_H_ACTIVE_PIXELS; x++) {
            if (x == dx || x == dx + 1) {
                s_pat_diag[row][x / 4] |= 0xffu << ((x % 4) * 8);
            }
        }
    }
    s_pat_ready = true;
}

static void __not_in_flash_func(render_test_line)(uint32_t row,
                                                  uint32_t *out) {
    if (row < 32) {
        /* Status: red = cumulative underruns. Keep this diagnostic
         * deterministic; render-time measurement is not part of the
         * picture-under-test. */
        uint32_t red = s_scanout.underruns;
        if (red > SCANOUT_H_ACTIVE_PIXELS) red = SCANOUT_H_ACTIVE_PIXELS;
        uint32_t red_word = render332_rgb(0xff0000) * 0x01010101u;
        uint32_t bg_word = pattern_grey(0x30) * 0x01010101u;
        for (uint32_t w = 0; w < SCANOUT_WORDS_PER_LINE; w++) {
            uint32_t x = w * 4;
            out[w] = x < red ? red_word : bg_word;
        }
    } else if (row < 96) {
        memcpy(out, s_pat[PAT_BARS], sizeof(s_pat[0]));
    } else if (row < 160) {
        memcpy(out, s_pat[PAT_STRIPES], sizeof(s_pat[0]));
    } else if (row < 224) {
        memcpy(out, s_pat[PAT_2ON1OFF], sizeof(s_pat[0]));
    } else if (row < 288) {
        memcpy(out, s_pat[PAT_HRAMP], sizeof(s_pat[0]));
    } else if (row < 352) {
        /* Vertical ramp: one level per row. */
        uint32_t g = pattern_grey((row * 255) / (RENDER_OUT_HEIGHT - 1));
        g *= 0x01010101u;
        for (uint32_t w = 0; w < SCANOUT_WORDS_PER_LINE; w++) {
            out[w] = g;
        }
    } else if (row < 416) {
        memcpy(out, s_pat[PAT_GRID], sizeof(s_pat[0]));
    } else {
        /* Copy the precomputed diagonal, then overlay the moving bar.
         * Keeping this path to a copy plus ten words ensures the test
         * pattern itself cannot starve the row producer. */
        memcpy(out, s_pat_diag[row - 416], sizeof(s_pat[0]));
        uint32_t pos = (s_scanout.frame * 4u) % SCANOUT_H_ACTIVE_PIXELS;
        for (uint32_t x = pos; x < pos + 40; x++) {
            uint32_t px = x % SCANOUT_H_ACTIVE_PIXELS;
            out[px / 4] |= 0xffu << ((px % 4) * 8);
        }
    }

    /* 1 px white border on top of the band content. */
    if (row == 0 || row == RENDER_OUT_HEIGHT - 1) {
        for (uint32_t w = 0; w < SCANOUT_WORDS_PER_LINE; w++) {
            out[w] = 0xffffffffu;
        }
    } else {
        out[0] |= 0xffu;
        out[SCANOUT_WORDS_PER_LINE - 1] |= 0xff000000u;
    }
}

#endif /* SPICOMPUTER_VIDEO_TEST_PATTERN */


/* Render the rows the ring has room for, using the frame geometry the
 * sequencer selected at the last frame boundary. A frame boundary can
 * pass mid-render (the DMA IRQ preempts), which resets rows_published;
 * scanout_ring_publish() detects that and drops the stale row, and the
 * loop re-reads the geometry for the new frame. */
static void __not_in_flash_func(render_rows)(void) {
    if (s_drain_pending) {
        /* New frame: apply the ops core 1 queued while the old one was
         * on screen, then render from the (possibly new) slot. */
        s_drain_pending = false;
        if (video_ops_drain()) {
            render332_invalidate_palette();
        }
    }

    const video_state_t *video = video_screen();
    while (scanout_ring_can_publish(&s_scanout)) {
        uint32_t row = s_scanout.rows_published;
        uint32_t *dst = scanout_ring_next(&s_scanout);

#if defined(SPICOMPUTER_VIDEO_TEST_PATTERN)
        (void)video;
        /* One pattern row per output line (geometry forced above). */
        render_test_line(row, dst);
#else
        render_line_332(video, (int)row, dst);
#endif
        if (!scanout_ring_publish(&s_scanout, row)) {
            break; /* a frame boundary passed: re-read the geometry */
        }
    }
}

/* Main-loop render pump: called from core 0 with interrupts enabled, so
 * the DMA IRQ preempts it freely. */
void video_hw_poll(void) {
    if (!s_render_pending) {
        return;
    }
    s_render_pending = false;
    render_rows();
}

/* ------------------------------------------------------------------ */
/* HSTX / DMA bring-up                                                */
/* ------------------------------------------------------------------ */

/* HSTX output bit indices are GPIO - 12 (the peripheral is fixed to
 * GP12-19); the differential pairs come from board_config.h. */
#define HSTX_BIT(pin) ((pin) - 12)
_Static_assert(HSTX_D0_P_PIN >= 12 && HSTX_D1_N_PIN <= 19 &&
                   HSTX_D0_P_PIN != HSTX_CLK_P_PIN,
               "HSTX pins must be the fixed GP12-19 pairs");

/* Target HSTX clock. The pixel clock is clk_hstx / 5 and the TMDS bit
 * clock is 10x the pixel clock, so 126 MHz gives the 25.2 MHz pixels and
 * 252 Mbps the 640x480 timing calls for. */
#define HSTX_CLK_HZ (126u * 1000u * 1000u)

/* Recorded for the OS core to report (core 0 owns no stdio). */
static uint32_t s_hstx_div = 1;
static uint32_t s_hstx_hz;
static uint32_t s_hstx_warn;

static void configure_hstx_clock(void) {
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t best_div = 1;
    uint32_t best_err = UINT32_MAX;
    for (uint32_t div = 1; div <= 3; div++) {
        uint32_t freq = sys / div;
        uint32_t err = (freq > HSTX_CLK_HZ) ? freq - HSTX_CLK_HZ
                                            : HSTX_CLK_HZ - freq;
        if (err < best_err) {
            best_err = err;
            best_div = div;
        }
    }
    clock_configure_int_divider(clk_hstx, 0,
                                CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                                sys, best_div);
    s_hstx_div = best_div;
    s_hstx_hz = clock_get_hz(clk_hstx);
    /* A clk_sys that divides to something far from 25.2 MHz pixels means
     * the display refresh is off spec; flag it for the boot log. */
    s_hstx_warn = (best_err > HSTX_CLK_HZ / 100) ? 1u : 0u;
}

void video_hw_clock_info(uint32_t *divisor, uint32_t *hstx_hz,
                         uint32_t *pixel_hz, bool *warn) {
    if (divisor) *divisor = s_hstx_div;
    if (hstx_hz) *hstx_hz = s_hstx_hz;
    if (pixel_hz) *pixel_hz = s_hstx_hz / 5; /* CSR CLKDIV = 5 */
    if (warn) *warn = s_hstx_warn != 0;
}

static void configure_hstx(void) {
    /* RGB332: 3 bits blue (L2), 3 bits green (L1), 2 bits red (L0),
     * matching render332_rgb() and the byte order in render332.c. */
    hstx_ctrl_hw->expand_tmds =
        2u << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0u << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
        2u << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29u << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
        1u << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26u << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;
    hstx_ctrl_hw->expand_shift =
        4u << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8u << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1u << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0u << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    /* Clock divider 5, pop every 5 cycles, shift 2 bits per cycle. */
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS |
                        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
                        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
                        2u << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

    /* Clock pair on the clock pins, data lanes on their pin pairs. */
    static const uint8_t lane_p[3] = {HSTX_D0_P_PIN, HSTX_D1_P_PIN,
                                      HSTX_D2_P_PIN};
    static const uint8_t lane_n[3] = {HSTX_D0_N_PIN, HSTX_D1_N_PIN,
                                      HSTX_D2_N_PIN};
    hstx_ctrl_hw->bit[HSTX_BIT(HSTX_CLK_P_PIN)] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[HSTX_BIT(HSTX_CLK_N_PIN)] =
        HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    for (unsigned lane = 0; lane < 3; lane++) {
        uint32_t sel = (lane * 10u) << HSTX_CTRL_BIT0_SEL_P_LSB |
                       (lane * 10u + 1u) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[HSTX_BIT(lane_p[lane])] = sel;
        hstx_ctrl_hw->bit[HSTX_BIT(lane_n[lane])] =
            sel | HSTX_CTRL_BIT0_INV_BITS;
    }

    for (unsigned gpio = 12; gpio <= 19; gpio++) {
        gpio_set_function(gpio, GPIO_FUNC_HSTX);
    }
}

static void configure_dma(void) {
    /* Two channels in a ping/pong chain: transfer one scanline to the
     * HSTX FIFO, then trigger the other. Claimed dynamically so they
     * cannot collide with the SD driver's pair. */
    s_dma_ping = (int)dma_claim_unused_channel(true);
    s_dma_pong = (int)dma_claim_unused_channel(true);

    dma_channel_config config =
        dma_channel_get_default_config((uint)s_dma_ping);
    channel_config_set_chain_to(&config, (uint)s_dma_pong);
    channel_config_set_dreq(&config, DREQ_HSTX);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, true);
    channel_config_set_write_increment(&config, false);
    dma_channel_configure((uint)s_dma_ping, &config, &hstx_fifo_hw->fifo,
                          s_vblank_vsync_off, 7, false);

    config = dma_channel_get_default_config((uint)s_dma_pong);
    channel_config_set_chain_to(&config, (uint)s_dma_ping);
    channel_config_set_dreq(&config, DREQ_HSTX);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, true);
    channel_config_set_write_increment(&config, false);
    dma_channel_configure((uint)s_dma_pong, &config, &hstx_fifo_hw->fifo,
                          s_vblank_vsync_off, 7, false);
}

static void enable_video_irqs(void) {
    dma_hw->ints2 = (1u << (uint)s_dma_ping) | (1u << (uint)s_dma_pong);
    dma_hw->inte2 = (1u << (uint)s_dma_ping) | (1u << (uint)s_dma_pong);
    irq_set_exclusive_handler(DMA_IRQ_2, hstx_dma_irq);
    irq_set_priority(DMA_IRQ_2, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(DMA_IRQ_2, true);
}

void video_hw_init(void) {
    if (s_started) {
        return;
    }
    s_started = true;

    /* Reset in case of a watchdog reboot with HSTX mid-frame. */
    reset_unreset_block_num_wait_blocking(RESET_HSTX);

    render332_init();
    /* Core 0 owns the display state; core 1 only queues ops for it. */
    video_screens_init();
#if defined(SPICOMPUTER_VIDEO_TEST_PATTERN)
    pattern_init(); /* build the static pattern rows before the DMA runs */
#endif

    scanout_init(&s_scanout);
    s_scanout.vblank_on = s_vblank_vsync_on;
    s_scanout.vblank_off = s_vblank_vsync_off;
    s_scanout.cmdlist = s_vactive_cmdlist;
    for (int slot = 0; slot < SCANOUT_RING_LINES; slot++) {
        s_scanout.ring[slot] = s_ring[slot];
        memset(s_scanout.ring[slot], 0, sizeof(s_ring[0]));
    }
    /* The ring is zeroed (black) and rows_published is 0, so the first
     * frame shows the active slot until the render pump has data: the
     * scanout is live before core 1 (and any program) exists. */
    scanout_frame_begin(&s_scanout);

    /* Clock the peripheral before enabling its CSR. HSTX is clocked
     * from clk_sys; the divider lands closest to HSTX_CLK_HZ because the
     * pixel clock is clk_hstx / 5. At the default 126 MHz clk_sys that
     * is /1 (25.2 MHz pixels); at an overclock such as 378 MHz it is
     * /3. */
    configure_hstx_clock();
    configure_hstx();

    configure_dma();
    enable_video_irqs();

    bus_ctrl_hw->priority =
        BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    /* Start the chain on ping; the IRQs take over from here. */
    s_scanout.channel_pong = false;
    dma_channel_start((uint)s_dma_ping);
}

uint32_t video_hw_frame_count(void) {
    return s_scanout.frame;
}

uint32_t video_hw_scanline(void) {
    return s_scanout.scanline;
}

uint32_t video_hw_underruns(void) {
    return s_scanout.underruns;
}

uint32_t video_hw_skews(void) {
    return s_diag_skews;
}

uint32_t video_hw_last_frame_steps(void) {
    return s_diag_last_steps;
}

uint32_t video_hw_gap_max_us(void) {
    return s_diag_gap_max_us;
}

uint32_t video_hw_long_gaps(void) {
    return s_diag_long_gaps;
}

uint32_t video_hw_fifo_empty(void) {
    return s_diag_fifo_empty;
}

uint32_t video_hw_fifo_wofs(void) {
    return s_diag_fifo_wofs;
}

uint32_t video_hw_last_gap_us(void) {
    return s_diag_evt_gap_us;
}

uint32_t video_hw_last_gap_frame(void) {
    return s_diag_evt_frame;
}

uint32_t video_hw_last_gap_line(void) {
    return s_diag_evt_line;
}

uint32_t video_hw_last_gap_fifo_level(void) {
    return s_diag_evt_fifo;
}

uint32_t video_hw_last_gap_cmdlist(void) {
    return s_diag_evt_cmdlist;
}

uint32_t video_hw_last_gap_underruns(void) {
    return s_diag_evt_underruns;
}

#else

void video_hw_init(void) {}
void video_hw_poll(void) {}
uint32_t video_hw_frame_count(void) { return 0; }
uint32_t video_hw_scanline(void) { return 0; }
uint32_t video_hw_underruns(void) { return 0; }
uint32_t video_hw_black_rows(void) { return 0; }
uint32_t video_hw_snapshot_fails(void) { return 0; }
uint32_t video_hw_skews(void) { return 0; }
uint32_t video_hw_last_frame_steps(void) { return 0; }
uint32_t video_hw_gap_max_us(void) { return 0; }
uint32_t video_hw_long_gaps(void) { return 0; }
uint32_t video_hw_fifo_empty(void) { return 0; }
uint32_t video_hw_fifo_wofs(void) { return 0; }
uint32_t video_hw_last_gap_us(void) { return 0; }
uint32_t video_hw_last_gap_frame(void) { return 0; }
uint32_t video_hw_last_gap_line(void) { return 0; }
uint32_t video_hw_last_gap_fifo_level(void) { return 0; }
uint32_t video_hw_last_gap_cmdlist(void) { return 0; }
uint32_t video_hw_last_gap_underruns(void) { return 0; }
void video_hw_clock_info(uint32_t *divisor, uint32_t *hstx_hz,
                         uint32_t *pixel_hz, bool *warn) {
    (void)divisor;
    (void)hstx_hz;
    (void)pixel_hz;
    (void)warn;
}

#endif

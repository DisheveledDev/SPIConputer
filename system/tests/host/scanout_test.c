/* scanout_test.c
 *
 * Host-side tests for the Phase 7 scanout pieces: the scanline
 * sequencer state machine (scanout.c against a memory trace) and the
 * RGB332 renderer (render332.c against the RGB888 reference in
 * render.c).
 *
 * The sequencer tests model the DMA pipeline and verify the *data* that
 * reaches the scanout, not just the post sequence:
 *   - the producer writes a tag (row + 1) into every word of a slot, so
 *     a post for row r must find tag r + 1;
 *   - a posted buffer stays in flight for two more posts (the other
 *     channel's command list is transferred first), so its contents must
 *     not change while it is in flight;
 *   - the producer must never run further ahead than
 *     SCANOUT_RING_AHEAD rows, and a frame boundary must reset both
 *     counters and refresh the frame geometry.
 *
 * Build and run:
 *   cmake -S tests/host -B build-host
 *   cmake --build build-host
 *   ./build-host/spicomputer_scanout_tests
 */
#include <stdio.h>
#include <string.h>

#include "render.h"
#include "render332.h"
#include "scanout.h"
#include "video.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* DMA trace sink: replaces the registers on the host.                */
/* ------------------------------------------------------------------ */

#define TRACE_MAX 1400
#define INFLIGHT_MAX 4

enum {
    SRC_VSYNC_ON = -1,
    SRC_VSYNC_OFF = -2,
    SRC_CMDLIST = -3,
    SRC_UNKNOWN = -4,
};

typedef struct {
    const uint32_t *src;
    uint32_t tag; /* value the slot must hold while in flight */
    int age;      /* posts since it was armed */
} inflight_t;

static const uint32_t g_cmd_vsync_on = 0x1111;
static const uint32_t g_cmd_vsync_off = 0x2222;
static const uint32_t g_cmd_cmdlist = 0x3333;
static uint32_t g_ring[SCANOUT_RING_LINES][SCANOUT_WORDS_PER_LINE];

static scanout_t g_s;
static int g_trace_len;
static inflight_t g_inflight[INFLIGHT_MAX];
static int g_inflight_used;
static int g_data_failures;
static int g_inflight_failures;
static int g_cmdlist_posts;
static int g_blank_posts;

static int trace_slot(const uint32_t *src) {
    if (src == &g_cmd_vsync_on) return SRC_VSYNC_ON;
    if (src == &g_cmd_vsync_off) return SRC_VSYNC_OFF;
    if (src == &g_cmd_cmdlist) return SRC_CMDLIST;
    for (int i = 0; i < SCANOUT_RING_LINES; i++) {
        if (src == g_ring[i]) {
            return i;
        }
    }
    return SRC_UNKNOWN;
}

/* The hook replaces the DMA registers. It also models what the hardware
 * does with the buffer contents: read every word. */
void scanout_hw_post(unsigned channel, const uint32_t *src, uint32_t words) {
    (void)channel;
    (void)words;
    g_trace_len++;

    /* Anything still in flight must not have been overwritten. */
    for (int i = 0; i < g_inflight_used; i++) {
        if (g_inflight[i].src[0] != g_inflight[i].tag) {
            g_inflight_failures++;
        }
    }
    for (int i = 0; i < g_inflight_used;) {
        if (++g_inflight[i].age > 2) {
            g_inflight[i] = g_inflight[g_inflight_used - 1];
            g_inflight_used--;
        } else {
            i++;
        }
    }

    int slot = trace_slot(src);
    if (slot == SRC_CMDLIST) {
        g_cmdlist_posts++;
    } else if (slot == SRC_VSYNC_ON || slot == SRC_VSYNC_OFF) {
        g_blank_posts++;
    } else if (slot >= 0) {
        /* A pixel post must carry the row the sequencer selected. */
        uint32_t tag = g_s.posted_row + 1u;
        if (src[0] != tag) {
            g_data_failures++;
        }
        if (g_inflight_used < INFLIGHT_MAX) {
            g_inflight[g_inflight_used].src = src;
            g_inflight[g_inflight_used].tag = tag;
            g_inflight[g_inflight_used].age = 0;
            g_inflight_used++;
        }
    }
}

/* The sequencer tests fix the frame geometry; the mode-change test
 * switches it between frames. */
static bool g_hook_2x = true;

void scanout_frame_begin(scanout_t *s) {
    s->rows_total = g_hook_2x ? RENDER_OUT_HEIGHT / 2 : RENDER_OUT_HEIGHT;
    s->row_2x = g_hook_2x;
}

static void scanout_reset(void) {
    g_hook_2x = true;
    scanout_init(&g_s);
    g_s.vblank_on = &g_cmd_vsync_on;
    g_s.vblank_off = &g_cmd_vsync_off;
    g_s.cmdlist = &g_cmd_cmdlist;
    memset(g_ring, 0, sizeof(g_ring));
    for (int i = 0; i < SCANOUT_RING_LINES; i++) {
        g_s.ring[i] = g_ring[i];
    }
    g_trace_len = 0;
    g_inflight_used = 0;
    g_data_failures = 0;
    g_inflight_failures = 0;
    g_cmdlist_posts = 0;
    g_blank_posts = 0;
    scanout_frame_begin(&g_s); /* as the driver does at init */
}

/* Producer: render rows while the ring allows, tagging each slot with
 * its row number (+1, so a tag is never zero and cannot match the
 * zeroed ring by accident). */
static void producer_tick(void) {
    while (scanout_ring_can_publish(&g_s)) {
        uint32_t row = g_s.rows_published;
        uint32_t *dst = scanout_ring_next(&g_s);
        for (int i = 0; i < SCANOUT_WORDS_PER_LINE; i++) {
            dst[i] = row + 1u;
        }
        if (!scanout_ring_publish(&g_s, row)) {
            break; /* a frame boundary passed mid-render */
        }
    }
}

/* One DMA completion: the sequencer posts, then the render ISR runs (the
 * hardware order: the IRQ wakes the renderer after posting). */
static void step(void) {
    scanout_step(&g_s);
    producer_tick();
    if (g_s.rows_published - g_s.rows_consumed > SCANOUT_RING_AHEAD) {
        CHECK(0, "producer ran ahead of the ring budget");
    }
}

/* Run to the end of the first active region, where the sequencer
 * rebases the ring (frame_active drops). Note that rows_consumed resets
 * there, so it is not a usable loop condition. */
static void run_to_rebase(void) {
    while (g_s.frame < 1 || g_s.frame_active) {
        step();
    }
}

/* ------------- test 1: frame structure and pixel data ------------- */

static void test_frame_structure(void) {
    scanout_reset();

    /* Run to the first active line's cmdlist. */
    while (!g_s.post_cmdlist) {
        step();
    }
    g_trace_len = 0;
    g_cmdlist_posts = 0;
    g_blank_posts = 0;
    g_data_failures = 0;
    g_inflight_failures = 0;
    g_inflight_used = 0;

    /* One frame: 480 cmdlist posts, 480 pixel posts, 43 vsync-off and 2
     * vsync-on blanking posts. */
    for (int i = 0; i < 1005; i++) {
        step();
    }

    CHECK(g_trace_len == 1005, "one post per DMA completion");
    CHECK(g_s.frame == 1, "one frame completed");
    CHECK(g_s.underruns == 0, "no underruns with a keeping-up producer");
    CHECK(g_cmdlist_posts == SCANOUT_V_ACTIVE_LINES,
          "one cmdlist per active line");
    CHECK(g_blank_posts == SCANOUT_V_FRONT_PORCH + SCANOUT_V_SYNC_WIDTH +
                               SCANOUT_V_BACK_PORCH,
          "45 blanking posts per frame");
    CHECK(g_data_failures == 0,
          "every pixel post carried the row the sequencer selected");
    CHECK(g_inflight_failures == 0,
          "no posted buffer was overwritten while in flight");
}

/* ------------- test 2: vblank prefetch is bounded ------------- */

static void test_prefetch_bound(void) {
    scanout_reset();

    run_to_rebase();
    CHECK(g_s.rows_consumed == 0, "rebase resets the consumer");
    CHECK(g_s.rows_published <= SCANOUT_RING_AHEAD,
          "the prefetch cannot run away after the rebase");

    /* vblank: the producer may prefetch, but only SCANOUT_RING_AHEAD
     * rows, never a whole frame. */
    for (int i = 0; i < 45; i++) {
        step();
    }
    CHECK(g_s.rows_published == SCANOUT_RING_AHEAD,
          "vblank prefetch fills exactly the ring budget");
    for (uint32_t row = 0; row < g_s.rows_published; row++) {
        CHECK(g_ring[row % SCANOUT_RING_LINES][0] == row + 1,
              "prefetched slot holds its own row");
    }
}

/* ------------- test 3: 1x and 2x cadences ------------- */

static void check_cadence(bool two_x) {
    scanout_reset();
    g_hook_2x = two_x;
    scanout_frame_begin(&g_s);

    while (!g_s.post_cmdlist) {
        step();
    }
    g_data_failures = 0;
    g_inflight_failures = 0;

    for (int i = 0; i < 1005; i++) {
        step();
    }
    CHECK(g_s.underruns == 0, "cadence frame has no underruns");
    CHECK(g_data_failures == 0, "cadence pixel data is correct");
    CHECK(g_inflight_failures == 0, "cadence has no in-flight overwrite");
}

static void test_cadences(void) {
    check_cadence(true);
    check_cadence(false);
}

/* ------------- test 4: underruns freeze on the last row ------------- */

static void test_underruns(void) {
    scanout_reset();

    /* Let the producer fill what it can, then stop it. */
    while (!g_s.post_cmdlist) {
        step();
    }
    uint32_t published = g_s.rows_published;

    g_inflight_failures = 0;
    for (int i = 0; i < 1005; i++) {
        scanout_step(&g_s); /* no producer */
    }

    CHECK(g_s.underruns > 0, "stalled producer causes underruns");
    CHECK(published <= SCANOUT_RING_AHEAD, "producer started within budget");
    CHECK(g_inflight_failures == 0,
          "frozen picture is not overwritten while in flight");
}

/* ------------- test 5: a mode change is picked up at the rebase -------- */

static void test_mode_change(void) {
    scanout_reset(); /* 2x */

    /* Run to the last row of the active region, then switch mode and
     * cross the frame boundary: the rebase must pick up the new
     * geometry before the vblank prefetch uses it. */
    while (!(g_s.frame_active &&
             g_s.rows_consumed + 1u >= g_s.rows_total)) {
        step();
    }
    CHECK(g_s.row_2x && g_s.rows_total == RENDER_OUT_HEIGHT / 2,
          "2x geometry during the frame");

    g_hook_2x = false;
    while (g_s.frame_active) {
        step();
    }
    CHECK(!g_s.row_2x && g_s.rows_total == RENDER_OUT_HEIGHT,
          "the rebased frame uses the new mode");
}

/* ------------- test 6: RGB332 conversion (colour order) ------------- */

static uint8_t expected332(uint32_t rgb888) {
    uint8_t r = (uint8_t)(rgb888 >> 16), g = (uint8_t)(rgb888 >> 8),
            b = (uint8_t)rgb888;
    return (uint8_t)((r & 0xe0u) | ((g & 0xe0u) >> 3) | ((b & 0xc0u) >> 6));
}

static void test_rgb332(void) {
    CHECK(render332_rgb(0xff0000) == 0xe0, "red is bits 7:5");
    CHECK(render332_rgb(0x00ff00) == 0x1c, "green is bits 4:2");
    CHECK(render332_rgb(0x0000ff) == 0x03, "blue is bits 1:0");
    CHECK(render332_rgb(0xffffff) == 0xff, "white is all ones");
    CHECK(render332_rgb(0x000000) == 0x00, "black is zero");
    CHECK(render332_rgb(0x123456) == expected332(0x123456),
          "mixed colour conversion");
}

/* ------------- test 7: render332 matches the reference renderer -------- */

static void test_render332_matches_reference(void) {
    video_state_t v;
    uint8_t ref[RENDER_LINE_BYTES];
    uint32_t words[RENDER332_WORDS_PER_LINE];
    const int modes[] = {VIDEO_MODE_TEXT40, VIDEO_MODE_TEXT40C,
                         VIDEO_MODE_TEXT80, VIDEO_MODE_TEXT80C,
                         VIDEO_MODE_PIXEL};

    render332_init();
    for (int m = 0; m < (int)(sizeof(modes) / sizeof(modes[0])); m++) {
        video_state_init(&v);
        CHECK(video_set_mode(&v, modes[m]), "set video mode");

        for (int i = 0; i < VIDEO_COLS * VIDEO_ROWS; i++) {
            v.char_map[0][i] = (uint8_t)('A' + (i * 7) % 26);
            v.attr_map[0][i] = (uint8_t)(i % 2 ? 0x85 : 0x03);
            v.attr_map[1][i] = (uint8_t)(i % 5 == 0 ? 0 : 0x40);
            v.char_map[1][i] = (uint8_t)('!' + (i * 3) % 20);
            v.attr_map[2][i] = 0x40; /* transparent */
        }
        for (int i = 0; i < 256; i++) {
            v.palette[i] = (uint32_t)(i * 0x010101) | 0x001020u;
        }
        if (v.mode == VIDEO_MODE_PIXEL) {
            /* Vary by column *and* row: patterns that repeat every
             * 256 bytes can hide a wrong framebuffer row. */
            for (int i = 0; i < VIDEO_FB_COLS * VIDEO_FB_ROWS; i++) {
                int col = i % VIDEO_FB_COLS;
                int line = i / VIDEO_FB_COLS;
                v.framebuf[i] = (uint8_t)(col * 3 + line * 11);
            }
        }

        for (int y = 0; y < RENDER_OUT_HEIGHT; y += 29) {
            render_line(&v, y, ref);
            render_line_332(&v, y, words);

            for (int x = 0; x < RENDER_OUT_WIDTH; x += 29) {
                uint32_t ref_rgb = ((uint32_t)ref[x * 3] << 16) |
                                   ((uint32_t)ref[x * 3 + 1] << 8) |
                                   ref[x * 3 + 2];
                uint8_t got =
                    (uint8_t)((words[x / 4] >> ((x % 4) * 8)) & 0xff);
                uint8_t want = expected332(ref_rgb);
                CHECK(got == want, "render332 pixel matches reference");
            }
        }

        CHECK(render332_is_2x(&v) ==
                  (modes[m] != VIDEO_MODE_TEXT80 &&
                   modes[m] != VIDEO_MODE_TEXT80C),
              "2x classification");
    }
}

static void test_text_cell_masks(void) {
    video_state_t v;
    uint32_t words[RENDER332_WORDS_PER_LINE];

    render332_init();
    video_state_init(&v);
    v.char_map[0][0] = '#';
    v.palette[1] = 0xff0000;
    v.version++;
    render_line_332(&v, 0, words);
    CHECK(words[0] == 0xe0e00000u, "2x cell first word preserves pixel order");
    CHECK(words[1] == 0x0000e0e0u, "2x cell second word preserves pixel order");
    CHECK(words[2] == 0xe0e0e0e0u, "2x cell third word preserves pixel order");
    CHECK(words[3] == 0x00000000u, "2x cell fourth word preserves pixel order");

    CHECK(video_set_mode(&v, VIDEO_MODE_TEXT80), "switch to 1x text mode");
    v.char_map[0][0] = '#';
    v.palette[1] = 0xff0000;
    v.version++;
    render_line_332(&v, 0, words);
    CHECK(words[0] == 0x00e0e000u, "1x cell first word preserves pixel order");
    CHECK(words[1] == 0x0000e0e0u, "1x cell second word preserves pixel order");
}

/* ------------- test 8: palette version cache ------------- */

static void test_palette_cache(void) {
    video_state_t v;
    uint32_t words[RENDER332_WORDS_PER_LINE];

    render332_init();
    video_state_init(&v);
    v.char_map[0][0] = '#';
    v.attr_map[0][0] = 0x00;

    v.palette[1] = 0xff0000;
    v.version++;
    render_line_332(&v, 0, words);
    uint32_t red = words[2];

    v.palette[1] = 0x0000ff;
    render_line_332(&v, 0, words);
    CHECK(words[2] == red, "palette LUT stays cached without a version bump");

    v.version++;
    render_line_332(&v, 0, words);
    CHECK(words[2] != red, "palette change is picked up via version");
}

int main(void) {
    setbuf(stdout, NULL);
    test_frame_structure();
    test_prefetch_bound();
    test_cadences();
    test_underruns();
    test_mode_change();
    test_rgb332();
    test_text_cell_masks();
    test_render332_matches_reference();
    test_palette_cache();

    if (g_failures) {
        printf("%d FAILURES\n", g_failures);
        return 1;
    }
    printf("scanout tests passed\n");
    return 0;
}

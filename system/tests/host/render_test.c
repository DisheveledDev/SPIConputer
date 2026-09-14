/* render_test.c
 *
 * Host-side tests for the video pieces: the RGB888 golden renderer
 * (golden output, 2x scaling, attributes, custom tiles, mode 10), the
 * core 1 -> core 0 op queue, and the screen Lua module.
 *
 * Build and run:
 *   cmake -S tests/host -B build-host
 *   cmake --build build-host
 *   ./build-host/spicomputer_render_tests
 */
#include <stdio.h>
#include <string.h>

#include "fs_core0.h"
#include "lauxlib.h"
#include "lua.h"
#include "program.h"
#include "render.h"
#include "rpc.h"
#include "screen_lua.h"
#include "system_state.h"
#include "video.h"

extern void mock_set_file(const char *path, const char *content);

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void rpc_wait_host(void) {
    fs_core0_service();
}

/* ---------------- test 1: renderer golden output (mode 0) ---------------- */

static void test_render_mode0(void) {
    video_state_t v;
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    /* Char map cleared to spaces; put 'A' at (0,0).
     * font8x8 'A': rows 0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00.
     * Logical row 0: subline 0 = 0x0C = 0b00001100 -> pixels 2,3 on
     * (bit 0 is the leftmost pixel), 2x scaled -> outputs 4-7 white. */
    v.base_char[0] = 'A';
    render_line(&v, 0, line);
    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int on = (x >= 4 && x <= 7);
        uint8_t expect = on ? 0xff : 0x00;
        CHECK(line[x * 3] == expect && line[x * 3 + 1] == expect &&
                  line[x * 3 + 2] == expect,
              "row 0 'A' pixels with 2x scaling");
    }

    /* Logical row 2 -> subline 2 = 0x33 = 0b00110011: pixels 0,1,4,5 on,
     * 2x scaled -> outputs 0-3 and 8-11. */
    render_line(&v, 2, line);
    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int on = x <= 3 || (x >= 8 && x <= 11);
        uint8_t expect = on ? 0xff : 0x00;
        CHECK(line[x * 3] == expect && line[x * 3 + 1] == expect &&
                  line[x * 3 + 2] == expect,
              "row 2 'A' pixels with 2x scaling");
    }

    /* The scanout shows each logical row twice; the renderer produces
     * one line per logical row and the scanout repeats it. */
    render_line(&v, 4, line);
    CHECK(line[0] == 0xff && line[11 * 3] == 0xff && line[12 * 3] == 0x00,
          "row 4 subline 4 (0x3F)");
}

/* ---------------- test 2: attributes (mode 1) ---------------- */

static void test_render_attrs(void) {
    video_state_t v;
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    v.mode = VIDEO_MODE_TEXT40C;
    v.base_char[0] = 'A';
    /* 'A' subline 4 = 0x3F: pixels 0..5 on -> outputs 0..11. */
    v.base_attr[0] = 0x00; /* colour 0 -> palette[1] = white */
    render_line(&v, 4, line);
    CHECK(line[0] == 0xff && line[1] == 0xff && line[2] == 0xff,
          "colour 0 is default white");

    v.base_attr[0] = 0x02; /* colour 2 -> palette[3] = cyan 0xaaffee */
    render_line(&v, 4, line);
    CHECK(line[0] == 0xaa && line[1] == 0xff && line[2] == 0xee,
          "colour 2 is cyan");

    v.base_attr[0] = 0x80; /* invert: on pixels become background (black) */
    render_line(&v, 4, line);
    CHECK(line[0] == 0x00 && line[1] == 0x00 && line[2] == 0x00,
          "invert makes on-pixel black");
    CHECK(line[12 * 3] == 0xff, "invert makes off-pixel white");

    /* The overlay composites over the base: an untouched cell (bit 6 of
     * its attribute set) is transparent, a written cell wins, and hiding
     * it again brings the base back. */
    uint8_t base_line[RENDER_LINE_BYTES];
    memcpy(base_line, line, sizeof(base_line));
    v.overlay_char[0] = 'B';
    render_line(&v, 4, line);
    CHECK(memcmp(line, base_line, sizeof(line)) == 0,
          "unwritten overlay cell is transparent");
    v.overlay_attr[0] = 0x00; /* opaque, default white */
    render_line(&v, 4, line);
    CHECK(memcmp(line, base_line, sizeof(line)) != 0, "overlay is visible");
    v.overlay_attr[0] = VIDEO_ATTR_TRANSPARENT;
    render_line(&v, 4, line);
    CHECK(memcmp(line, base_line, sizeof(line)) == 0,
          "transparent overlay cell restores the base");
}

/* ---------------- test 3: custom tiles ---------------- */

static void test_render_custom_tile(void) {
    video_state_t v;
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    /* Override tile 200: row 0 = 0x0C -> pixels 2,3 on (2x -> 4..7),
     * row 1 = 0x01 -> pixel 0 on (2x -> 0,1), the rest blank. */
    v.tiles[200][0] = 0x0C;
    v.tiles[200][1] = 0x01;
    v.tile_defined[200] = 1;
    v.base_char[0] = 200;

    render_line(&v, 0, line);
    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int on = (x >= 4 && x <= 7);
        CHECK((line[x * 3] != 0) == on, "custom tile row 0 pattern");
    }
    render_line(&v, 1, line);
    CHECK(line[0] == 0xff && line[3] == 0xff && line[6] == 0x00,
          "custom tile row 1 pattern");
    CHECK(line[16 * 3] == 0x00, "next tile column blank");
}

/* ---------------- test 4: op queue routing ---------------- */

static void test_op_queue(void) {
    video_screens_init();
    video_state_t *s0 = video_screen();
    CHECK(video_screen_index() == 0, "slot 0 active after init");

    video_op_t out = {.op = VIDEO_OP_OUT, .a = 0, .b = 0, .c = 'K', .d = 0};
    video_op_t pal = {.op = VIDEO_OP_PALETTE, .a = 9, .d = 0x102030};
    video_op_t slot = {.op = VIDEO_OP_SLOT, .a = 1};
    video_op_t out1 = {.op = VIDEO_OP_OUT, .a = 1, .b = 0, .c = 'A', .d = 0};
    video_op_put(&out);
    video_op_put(&pal);
    video_op_put(&slot);
    video_op_put(&out1);

    CHECK(video_ops_drain(), "drain reports the palette change");
    CHECK(video_screen_index() == 1, "slot switch applied");
    CHECK(s0->base_char[0] == 'K', "op applied to the slot in force");
    CHECK(s0->palette[9] == 0x102030, "palette op applied to slot 0");
    CHECK(video_screen()->base_char[1] == 'A', "later ops follow the slot");
    CHECK(!video_ops_drain(), "empty drain reports no palette change");
    video_screens_init(); /* leave slot 0 active for the Lua test */
}

/* ---------------- test 5: mode 10 pixel buffer ---------------- */

static void test_render_mode10(void) {
    uint8_t line[RENDER_LINE_BYTES];

    video_screens_init();
    video_op_t mode = {.op = VIDEO_OP_MODE, .a = VIDEO_MODE_PIXEL};
    video_op_put(&mode);
    video_ops_drain();

    video_state_t *v = video_screen();
    CHECK(v->mode == VIDEO_MODE_PIXEL, "mode 10 applied");
    CHECK(v->framebuf != NULL, "mode 10 attaches the pixel buffer");

    v->palette[7] = 0x112233;
    video_op_t plot1 = {.op = VIDEO_OP_PLOT, .a = 0, .b = 0, .d = 7};
    video_op_t plot2 = {.op = VIDEO_OP_PLOT, .a = 1, .b = 0, .d = 7};
    video_op_put(&plot1);
    video_op_put(&plot2);
    video_ops_drain();

    render_line(v, 0, line);
    for (int x = 0; x < 4; x++) {
        CHECK(line[x * 3] == 0x11 && line[x * 3 + 1] == 0x22 &&
                  line[x * 3 + 2] == 0x33,
              "mode 10 pixel colour 2x scaled");
    }
    CHECK(line[4 * 3] == 0x00, "mode 10 background black");

    video_op_t text = {.op = VIDEO_OP_MODE, .a = VIDEO_MODE_TEXT40};
    video_op_put(&text);
    video_ops_drain();
    CHECK(v->framebuf == NULL, "leaving mode 10 detaches the pixel buffer");
    video_screens_init();
}

/* ---------------- test 6: screen module (via a program) ---------------- */

static const char *SCREEN_LUA =
    "function setup()\n"
    "  assert(ScreenMode(1) == true)\n"
    "  assert(ScreenOut(0, 0, 65, 0x02) == true)\n"
    "  assert(ScreenAttr(1, 1, 0x80) == true)\n"
    "  assert(ScreenDefineTile(200, {1,2,3,4,5,6,7,8}) == true)\n"
    "  assert(ScreenPalette(9, 16, 32, 48) == true)\n"
    "  assert(ScreenPaletteSet({{0,0,0},{255,0,0}}) == true)\n"
    "  assert(ScreenClear(32) == true)\n"
    "  assert(ScreenMode(10) == true)\n"
    "  assert(ScreenPlot(5, 6, 7) == true)\n"
    "  ExitProgram()\n"
    "end\n"
    "function tick() end\n";

static void test_screen_module(void) {
    video_screens_init();
    mock_set_file("scr.lua", SCREEN_LUA);
    CHECK(program_boot("scr.lua", NULL), "screen program boots");
    /* setup() runs at launch; it already exited. */
    program_scheduler_step(); /* cleanup */
    CHECK(program_top() == NULL, "screen program exited");

    /* The program's ops are still queued: apply them and check the
     * end state of the slot it owned. */
    video_ops_drain();
    video_state_t *v = video_screen();
    CHECK(video_lua_mode() == VIDEO_MODE_PIXEL, "mode 10 noted for the API");
    CHECK(v->mode == VIDEO_MODE_PIXEL, "mode 10 applied from Lua");
    CHECK(v->palette[9] == 0x102030, "ScreenPalette(9,16,32,48)");
    CHECK(v->palette[1] == 0xff0000, "ScreenPaletteSet entry 1");
    CHECK(v->framebuf && v->framebuf[6 * VIDEO_FB_COLS + 5] == 7,
          "ScreenPlot pixel");
    CHECK(v->tiles[200][0] == 1 && v->tiles[200][7] == 8,
          "ScreenDefineTile bytes");
    video_screens_init();
}

/* ---------------- test 7: overlay API (via a program) ---------------- */

static const char *OVERLAY_LUA =
    "function setup()\n"
    "  assert(ScreenMode(1) == true)\n"
    "  assert(ScreenOut(0, 0, 65) == true)\n"
    "  assert(OverlayOut(1, 1, 66, 0x03) == true)\n"
    "  assert(OverlayAttr(2, 1, 0x40) == true)\n"
    "  assert(OverlayClear() == true)\n"
    "  assert(OverlayOut(3, 2, 67, 0x05) == true)\n"
    "  ExitProgram()\n"
    "end\n"
    "function tick() end\n";

static void test_overlay_module(void) {
    video_screens_init();
    mock_set_file("ovl.lua", OVERLAY_LUA);
    CHECK(program_boot("ovl.lua", NULL), "overlay program boots");
    program_scheduler_step();
    CHECK(program_top() == NULL, "overlay program exited");
    video_ops_drain();

    const video_state_t *v = video_screen();
    CHECK(v->base_char[0] == 65, "ScreenOut writes the base layer");
    CHECK(v->overlay_attr[1 * VIDEO_COLS + 1] == VIDEO_ATTR_TRANSPARENT,
          "OverlayClear hides earlier overlay cells");
    CHECK(v->overlay_char[2 * VIDEO_COLS + 3] == 67 &&
              v->overlay_attr[2 * VIDEO_COLS + 3] == 0x05,
          "OverlayOut writes the overlay layer");
    video_screens_init();
}

int main(void) {
    printf("=== render / screen tests ===\n");
    rpc_bind_wait(rpc_wait_host);
    rpc_bind_signal(NULL);
    if (!fs_core0_mount()) {
        printf("FAIL: mock SD mount\n");
        return 1;
    }

    test_render_mode0();
    test_render_attrs();
    test_render_custom_tile();
    test_op_queue();
    test_render_mode10();
    test_screen_module();
    test_overlay_module();

    if (g_failures == 0) {
        printf("all render/screen tests passed\n");
        return 0;
    }
    printf("%d render/screen test(s) FAILED\n", g_failures);
    return 1;
}

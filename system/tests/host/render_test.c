/* render_test.c
 *
 * Host-side tests for the Phase 1 video pieces: the scanline renderer
 * (golden output, 2x scaling, attributes, custom tiles, mode 10), the
 * serial mirror text encoder, and the screen Lua module.
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
     * Line 0: subline 0 = 0x0C = 0b00001100 -> pixels 2,3 on (bit 0
     * is the leftmost pixel), 2x scaled -> outputs 4-7 white. */
    v.char_map[0][0] = 'A';
    render_line(&v, 0, line);
    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int on = (x >= 4 && x <= 7);
        uint8_t expect = on ? 0xff : 0x00;
        CHECK(line[x * 3] == expect && line[x * 3 + 1] == expect &&
                  line[x * 3 + 2] == expect,
              "line 0 'A' pixels with 2x scaling");
    }

    /* Line 4 = logical line 2 -> subline 2 = 0x33 = 0b00110011:
     * pixels 0,1,4,5 on, 2x scaled -> outputs 0-3 and 8-11. */
    render_line(&v, 4, line);
    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int on = x <= 3 || (x >= 8 && x <= 11);
        uint8_t expect = on ? 0xff : 0x00;
        CHECK(line[x * 3] == expect && line[x * 3 + 1] == expect &&
                  line[x * 3 + 2] == expect,
              "line 4 'A' pixels with 2x scaling");
    }

    /* Vertical doubling: lines 4 and 5 must be identical. */
    uint8_t line5[RENDER_LINE_BYTES];
    render_line(&v, 5, line5);
    CHECK(memcmp(line, line5, RENDER_LINE_BYTES) == 0,
          "2x vertical scaling doubles the line");

    /* Line 8 = logical line 4 -> subline 4 = 0x3F: outputs 0..11. */
    render_line(&v, 8, line);
    CHECK(line[0] == 0xff && line[11 * 3] == 0xff && line[12 * 3] == 0x00,
          "line 8 subline 4 (0x3F)");
}

/* ---------------- test 2: attributes (mode 1) ---------------- */

static void test_render_attrs(void) {
    video_state_t v;
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    video_set_mode(&v, VIDEO_MODE_TEXT40C);
    v.char_map[0][0] = 'A';
    /* 'A' subline 4 = 0x3F: pixels 0..5 on -> outputs 0..11. */
    v.attr_map[0][0] = 0x00; /* colour 0 -> palette[1] = white */
    render_line(&v, 4, line);
    CHECK(line[0] == 0xff && line[1] == 0xff && line[2] == 0xff,
          "colour 0 is default white");

    v.attr_map[0][0] = 0x02; /* colour 2 -> palette[3] = cyan 0xaaffee */
    render_line(&v, 4, line);
    CHECK(line[0] == 0xaa && line[1] == 0xff && line[2] == 0xee,
          "colour 2 is cyan");

    v.attr_map[0][0] = 0x80; /* invert: on pixels become background (black) */
    render_line(&v, 4, line);
    CHECK(line[0] == 0x00 && line[1] == 0x00 && line[2] == 0x00,
          "invert makes on-pixel black");
    CHECK(line[12 * 3] == 0xff, "invert makes off-pixel white");

    uint8_t base_line[RENDER_LINE_BYTES];
    memcpy(base_line, line, sizeof(base_line));
    v.char_map[1][0] = 'B';
    v.attr_map[1][0] = 0;
    CHECK(video_set_z_order(&v, 1), "select overlay layer");
    render_line(&v, 4, line);
    CHECK(memcmp(line, base_line, sizeof(line)) != 0, "overlay is visible");
    v.attr_map[1][0] = VIDEO_ATTR_TRANSPARENT;
    render_line(&v, 4, line);
    CHECK(memcmp(line, base_line, sizeof(line)) == 0,
          "transparent overlay restores the base layer");
}

/* ---------------- test 3: custom tiles + mode 2 native ---------------- */

static void test_render_custom_tile_and_mode2(void) {
    video_state_t v;
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    /* Override tile 200: row 0 = 0x0C -> pixels 2,3 on (2x -> 4..7),
     * row 1 = 0x01 -> pixel 0 on (2x -> 0,1), the rest blank. */
    v.tiles[200][0] = 0x0C;
    v.tiles[200][1] = 0x01;
    v.tile_defined[200] = 1;
    v.char_map[0][0] = 200;

    render_line(&v, 0, line);
    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int on = (x >= 4 && x <= 7);
        CHECK((line[x * 3] != 0) == on, "custom tile row 0 pattern");
    }
    render_line(&v, 2, line);
    CHECK(line[0] == 0xff && line[3] == 0xff && line[6] == 0x00,
          "custom tile row 1 pattern");
    CHECK(line[16 * 3] == 0x00, "next tile column blank");

    /* Mode 2 (80x60): native 1x scaling. 'A' at (0,0), subline 4 row
     * 0x3F -> pixels 0..5 on (native outputs 0..5). */
    video_set_mode(&v, VIDEO_MODE_TEXT80);
    v.char_map[0][0] = 'A';
    render_line(&v, 4, line);
    CHECK(line[0] == 0xff && line[5 * 3] == 0xff && line[6 * 3] == 0x00,
          "mode 2 native pixels 0..5");

    /* Column 79, same subline: outputs 79*8 .. 79*8+5 = 632..637. */
    v.char_map[0][79] = 'A';
    render_line(&v, 4, line);
    CHECK(line[(79 * 8) * 3] == 0xff && line[(79 * 8 + 5) * 3] == 0xff &&
              line[(79 * 8 + 6) * 3] == 0x00,
          "mode 2 column 79 pixels");
}

/* ---------------- test 4: mode 10 framebuffer ---------------- */

static void test_render_mode10(void) {
    video_state_t v;
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    CHECK(video_set_mode(&v, VIDEO_MODE_PIXEL), "mode 10 allocates");
    v.palette[7] = 0x112233;
    v.framebuf[0 * VIDEO_FB_COLS + 0] = 7; /* pixel (0,0) */
    v.framebuf[0 * VIDEO_FB_COLS + 1] = 7; /* pixel (1,0) */

    render_line(&v, 0, line);
    for (int x = 0; x < 4; x++) {
        CHECK(line[x * 3] == 0x11 && line[x * 3 + 1] == 0x22 &&
                  line[x * 3 + 2] == 0x33,
              "mode 10 pixel colour 2x scaled");
    }
    CHECK(line[4 * 3] == 0x00, "mode 10 background black");

    render_line(&v, 1, line); /* vertical doubling */
    CHECK(line[0] == 0x11, "mode 10 line doubled");

    video_state_free(&v);
    CHECK(v.framebuf == NULL, "mode 10 framebuffer freed");
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
    mock_set_file("scr.lua", SCREEN_LUA);
    CHECK(program_boot("scr.lua", NULL), "screen program boots");
    /* setup() runs at launch; it already exited. */
    program_scheduler_step(); /* cleanup */
    CHECK(program_top() == NULL, "screen program exited");
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
    test_render_custom_tile_and_mode2();
    test_render_mode10();
    test_screen_module();

    if (g_failures == 0) {
        printf("all render/screen tests passed\n");
        return 0;
    }
    printf("%d render/screen test(s) FAILED\n", g_failures);
    return 1;
}

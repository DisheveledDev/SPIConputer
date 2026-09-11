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
#include "mirror.h"
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
     * Line 0: subline 0 = 0x0C = 0b00001100 -> pixels 4,5 on, 2x
     * scaled -> outputs 8,9,10,11 white, rest black. */
    v.char_map[0] = 'A';
    render_line(&v, 0, line);
    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int on = (x >= 8 && x <= 11);
        uint8_t expect = on ? 0xff : 0x00;
        CHECK(line[x * 3] == expect && line[x * 3 + 1] == expect &&
                  line[x * 3 + 2] == expect,
              "line 0 'A' pixels with 2x scaling");
    }

    /* Line 4 = logical line 2 -> subline 2 = 0x33 = 0b00110011:
     * pixels 2,3,6,7 on, 2x scaled -> outputs 4-7 and 12-15. */
    render_line(&v, 4, line);
    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int on = (x >= 4 && x <= 7) || (x >= 12 && x <= 15);
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

    /* Line 8 = logical line 4 -> subline 4 = 0x3F: outputs 4..15. */
    render_line(&v, 8, line);
    CHECK(line[4 * 3] == 0xff && line[15 * 3] == 0xff &&
              line[16 * 3] == 0x00 && line[3 * 3] == 0x00,
          "line 8 subline 4 (0x3F)");
}

/* ---------------- test 2: attributes (mode 1) ---------------- */

static void test_render_attrs(void) {
    video_state_t v;
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    video_set_mode(&v, VIDEO_MODE_TEXT40C);
    v.char_map[0] = 'A';
    /* 'A' subline 4 = 0x09: pixel 3 on. Check colour + invert. */
    /* 'A' subline 4 = 0x3F: pixel 2 on -> output 4. */
    v.attr_map[0] = 0x00; /* colour 0 -> palette[1] = white */
    render_line(&v, 4, line);
    CHECK(line[4 * 3] == 0xff && line[4 * 3 + 1] == 0xff &&
              line[4 * 3 + 2] == 0xff,
          "colour 0 is default white");

    v.attr_map[0] = 0x02; /* colour 2 -> palette[3] = cyan 0xaaffee */
    render_line(&v, 4, line);
    CHECK(line[4 * 3] == 0xaa && line[4 * 3 + 1] == 0xff &&
              line[4 * 3 + 2] == 0xee,
          "colour 2 is cyan");

    v.attr_map[0] = 0x80; /* invert: on pixels become background (black) */
    render_line(&v, 4, line);
    CHECK(line[4 * 3] == 0x00 && line[4 * 3 + 1] == 0x00 &&
              line[4 * 3 + 2] == 0x00,
          "invert makes on-pixel black");
    CHECK(line[0] == 0xff, "invert makes off-pixel white");
}

/* ---------------- test 3: custom tiles + mode 2 native ---------------- */

static void test_render_custom_tile_and_mode2(void) {
    video_state_t v;
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    /* Override tile 200 with a solid first row. */
    for (int c = 0; c < 8; c++) {
        v.tiles[200][0][c] = 0xff;
    }
    v.tile_defined[200] = 1;
    v.char_map[0] = 200;

    render_line(&v, 0, line);
    /* subline 0, tile col 0: row 0xff -> first 8 pixels on, 2x -> 16. */
    for (int x = 0; x < 16; x++) {
        CHECK(line[x * 3] == 0xff, "custom tile pixel on");
    }
    CHECK(line[16 * 3] == 0x00, "custom tile pixel off after run");

    /* Mode 2 (80x60): native 1x scaling. 'A' at (0,0), subline 4 row
     * 0x3F -> pixels 2..7 on (native outputs 2..7). */
    video_set_mode(&v, VIDEO_MODE_TEXT80);
    v.char_map[0] = 'A';
    render_line(&v, 4, line);
    CHECK(line[2 * 3] == 0xff && line[1 * 3] == 0x00,
          "mode 2 native pixel 2");
    CHECK(line[7 * 3] == 0xff && line[8 * 3] == 0x00,
          "mode 2 native pixel 7");

    /* Column 79, same subline: outputs 79*8+2 .. 79*8+7 = 634..639. */
    v.char_map[79] = 'A';
    render_line(&v, 4, line);
    CHECK(line[(79 * 8 + 2) * 3] == 0xff && line[(79 * 8 + 7) * 3] == 0xff,
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

/* ---------------- test 5: mirror encoder ---------------- */

static void test_mirror_encoder(void) {
    video_state_t v;
    char buf[5120];

    video_state_init(&v);
    v.char_map[0] = 'A';
    v.char_map[1] = 0;
    v.char_map[2] = 255;

    int n = mirror_encode_frame(&v, buf, sizeof(buf));
    CHECK(n > 0, "frame encoded");
    CHECK(strncmp(buf, "data=65,0,255,", 14) == 0,
          "frame starts with data= and decimal values");
    CHECK(buf[n - 1] == '\n', "frame ends with LF");
    CHECK(strchr(buf, ',') != NULL, "values comma separated");

    uint8_t rows[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    n = mirror_encode_tile(65, rows, buf, sizeof(buf));
    CHECK(n > 0 && strcmp(buf, "tile=65,1,2,3,4,5,6,7,8\n") == 0,
          "tile line encoded");
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
    printf("=== render / mirror / screen tests ===\n");
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
    test_mirror_encoder();
    test_screen_module();

    if (g_failures == 0) {
        printf("all render/mirror/screen tests passed\n");
        return 0;
    }
    printf("%d render/mirror/screen test(s) FAILED\n", g_failures);
    return 1;
}

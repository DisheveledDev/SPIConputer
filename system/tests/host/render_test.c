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

/* Like the simulator: the tests are single-threaded, so a full queue or
 * a full staging ring is drained from inside the wait. Counted, so the
 * tests can check that ordinary drawing never has to wait. */
static int g_full_waits = 0;

void video_queue_full_hook(void) {
    g_full_waits++;
    video_ops_drain();
}

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
    video_op_t plot1 = {.op = VIDEO_OP_PLOT, .a = 7, .d = 0};
    video_op_t plot2 = {.op = VIDEO_OP_PLOT, .a = 7, .d = 1};
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
    CHECK(v->overlay_attr[1 * VIDEO_COLS_2X + 1] == VIDEO_ATTR_TRANSPARENT,
          "OverlayClear hides earlier overlay cells");
    CHECK(v->overlay_char[2 * VIDEO_COLS_2X + 3] == 67 &&
              v->overlay_attr[2 * VIDEO_COLS_2X + 3] == 0x05,
          "OverlayOut writes the overlay layer");
    video_screens_init();
}

/* ---------------- test 8: ROM font upper half ---------------- */

static void test_render_rom_font(void) {
    video_state_t v;
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    /* 0xC4 (box light horizontal): rows 3-4 fully set, others blank.
     * Before the 256-glyph ROM, 0xC4 aliased to 'D' (0x44). */
    v.base_char[0] = 0xC4;
    render_line(&v, 3, line);
    bool all_on = true;
    for (int x = 0; x < 16; x++) {
        all_on = all_on && line[x * 3] == 0xff;
    }
    CHECK(all_on, "0xC4 row 3 is a full horizontal line");
    render_line(&v, 0, line);
    CHECK(line[0] == 0x00 && line[7 * 3] == 0x00,
          "0xC4 row 0 is blank (no ASCII aliasing)");

    /* 0xDB (full block) fills every pixel; 0xDF (upper half) stops at
     * row 3. */
    v.base_char[0] = 0xDB;
    render_line(&v, 7, line);
    CHECK(line[0] == 0xff && line[15 * 3] == 0xff, "0xDB row 7 full");
    v.base_char[0] = 0xDF;
    render_line(&v, 4, line);
    CHECK(line[0] == 0x00, "0xDF row 4 blank");
    render_line(&v, 3, line);
    CHECK(line[0] == 0xff, "0xDF row 3 set");

    /* Undefined upper codes are blank, and a program can still define
     * a tile over any of them. */
    v.base_char[0] = 0x80;
    render_line(&v, 3, line);
    CHECK(line[0] == 0x00, "undefined code 0x80 renders blank");
}

/* ---------------- test 8b: the 80-column modes ---------------- */

static void test_render_mode80(void) {
    video_state_t v;
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    v.mode = VIDEO_MODE_TEXT80;
    /* 80 columns: cell (79, 59) is output pixels 632..639 on output
     * lines 472..479, drawn 1:1. 'A' row 0 = 0x0C -> pixels 2,3. */
    v.base_char[59 * VIDEO_COLS_1X + 79] = 'A';
    render_line(&v, 59 * 8, line);
    for (int x = 0; x < RENDER_OUT_WIDTH; x++) {
        int on = (x == 634 || x == 635);
        uint8_t expect = on ? 0xff : 0x00;
        CHECK(line[x * 3] == expect, "80-column 'A' at the last cell, 1x");
    }
    /* Row 1 of the same cell is on the very next output line. */
    render_line(&v, 59 * 8 + 1, line);
    CHECK(line[633 * 3] == 0xff && line[636 * 3] == 0xff && line[632 * 3] == 0x00,
          "80-column rows are one output line each");
    /* The 40-column stride would put that cell elsewhere: nothing at
     * (39, 59) in 40-column terms, i.e. output pixel 312. */
    CHECK(line[312 * 3] == 0x00, "80-column stride, not 40");

    /* Mode 2 ignores colour bits; mode 3 uses them, like 0 and 1. */
    v.base_char[1] = '#';
    v.base_attr[1] = 0x02; /* cyan in a colour mode */
    /* '#' row 0 = 0x36: pixels 1, 2, 4, 5 of the cell, so x = 9 is on. */
    render_line(&v, 0, line);
    CHECK(line[9 * 3] == 0xff && line[9 * 3 + 2] == 0xff && line[8 * 3] == 0x00,
          "mode 2 is B&W");
    v.mode = VIDEO_MODE_TEXT80C;
    render_line(&v, 0, line);
    CHECK(line[9 * 3] == 0xaa && line[9 * 3 + 1] == 0xff && line[9 * 3 + 2] == 0xee,
          "mode 3 colours cells");
    CHECK(line[16 * 3] == 0x00, "cell 2 blank at 1x (8 pixels per cell)");
}

/* The API works in the 80-column geometry: cells up to (79, 59), block
 * ops clipped at 80x60, a full-screen write of 4800 bytes. */
static const char *MODE80_LUA =
    "function setup()\n"
    "  assert(ScreenMode(3) == true)\n"
    "  assert(ScreenOut(79, 59, 65, 0x02) == true)\n"
    "  assert(ScreenOut(80, 0, 65) == nil)\n"
    "  assert(ScreenOut(0, 60, 65) == nil)\n"
    "  assert(OverlayBox(0, 0, 80, 60, 2) == true)\n"
    "  assert(ScreenWrite(0, 1, string.rep('x', 4800)) == true)\n"
    "  assert(ScreenWrite(78, 2, 'abc') == true)\n"
    "  assert(ScreenScroll(0, 0, 80, 60, 0, -1, 46) == true)\n"
    "  assert(ScreenMode(1) == true)\n"
    "  assert(ScreenOut(40, 0, 65) == nil)\n"
    "  assert(ScreenMode(3) == true)\n"
    "  assert(ScreenOut(40, 0, 66) == true)\n"
    "  ExitProgram()\n"
    "end\n"
    "function tick() end\n";

static void test_mode80_api(void) {
    video_screens_init();
    mock_set_file("m80.lua", MODE80_LUA);
    CHECK(program_boot("m80.lua", NULL), "80-column program boots");
    program_scheduler_step();
    CHECK(program_top() == NULL, "80-column program exited");
    video_ops_drain();
    const video_state_t *v = video_screen();
    CHECK(v->mode == VIDEO_MODE_TEXT80C, "mode 3 in force");
    CHECK(v->base_char[40] == 66, "cell (40, 0) exists in mode 3");
#define CELL80(x, y) ((y) * VIDEO_COLS_1X + (x))
    /* Before the final mode switches (which cleared the screen), the
     * scroll moved row 1's x's to row 0 and 'abc' wrapped from (78, 2)
     * to (0, 3), row 59 was 'A' at the corner: check the geometry that
     * survives, the scroll-fill row and the corner, in a second run. */
    CHECK(v->overlay_attr[CELL80(79, 59)] == VIDEO_ATTR_TRANSPARENT,
          "mode switch cleared the overlay");
#undef CELL80
    video_screens_init();
}

static const char *MODE80_LAYOUT_LUA =
    "function setup()\n"
    "  assert(ScreenMode(2) == true)\n"
    "  assert(ScreenWrite(0, 1, string.rep('x', 4800)) == true)\n"
    "  assert(ScreenWrite(78, 2, 'abc') == true)\n"
    "  assert(ScreenScroll(0, 0, 80, 60, 0, -1, 46) == true)\n"
    "  assert(ScreenOut(79, 59, 65, 0x02) == true)\n"
    "  assert(OverlayBox(0, 0, 80, 60, 2, 0x80) == true)\n"
    "  ExitProgram()\n"
    "end\n"
    "function tick() end\n";

static void test_mode80_layout(void) {
    video_screens_init();
    mock_set_file("m80b.lua", MODE80_LAYOUT_LUA);
    CHECK(program_boot("m80b.lua", NULL), "80-column layout program boots");
    program_scheduler_step();
    video_ops_drain();
    const video_state_t *v = video_screen();
#define CELL80(x, y) ((y) * VIDEO_COLS_1X + (x))
    CHECK(v->mode == VIDEO_MODE_TEXT80, "mode 2 in force");
    CHECK(v->base_char[CELL80(0, 0)] == 'x' && v->base_char[CELL80(79, 0)] == 'x',
          "scroll moved the full-width row up (80-cell rows)");
    CHECK(v->base_char[CELL80(78, 1)] == 'a' && v->base_char[CELL80(79, 1)] == 'b' &&
              v->base_char[CELL80(0, 2)] == 'c',
          "write wrapped at column 80, then scrolled up: 'a' at 78, 'b' 79, 'c' 0");
    CHECK(v->base_char[CELL80(0, 59)] == '.' && v->base_char[CELL80(79, 59)] == 'A',
          "scroll filled row 59, then the corner cell was written");
    CHECK(v->overlay_char[CELL80(79, 59)] == 0xBC && v->overlay_char[CELL80(0, 0)] == 0xC9,
          "overlay box spans the 80x60 screen");
    CHECK(v->overlay_attr[CELL80(40, 30)] == VIDEO_ATTR_TRANSPARENT,
          "inside the box stays transparent");
#undef CELL80
    video_screens_init();
}

/* ---------------- test 8c: mode 11 (160x120, 4x4 pixels) ---------------- */

static void test_render_mode11(void) {
    video_state_t v;
    static uint8_t fb[VIDEO_FB_COLS * VIDEO_FB_ROWS];
    uint8_t line[RENDER_LINE_BYTES];

    video_state_init(&v);
    v.mode = VIDEO_MODE_PIXEL_LO;
    v.framebuf = fb;
    for (int i = 0; i < VIDEO_FB_COLS * VIDEO_FB_ROWS; i++) fb[i] = 0;
    /* Default palette: 16 + 36r + 6g + b is the 6x6x6 cube, 232.. grey. */
    CHECK(v.palette[16 + 36 * 5] == 0xff0000 && v.palette[231] == 0xffffff &&
              v.palette[16 + 5] == 0x0000ff && v.palette[232] == 0x080808,
          "default 256-colour palette: cube and grey ramp");

    /* Pixel (0, 0) is output pixels 0..3 on logical rows 0 and 1;
     * pixel (159, 119) is 636..639 on logical rows 238 and 239. */
    fb[0] = 16 + 36 * 5;                     /* red */
    fb[119 * VIDEO_FB_LO_COLS + 159] = 231;  /* white */
    render_line(&v, 0, line);
    CHECK(line[0] == 0xff && line[1] == 0x00 && line[3 * 3] == 0xff && line[4 * 3] == 0x00,
          "mode 11 pixel 0 is four output pixels");
    render_line(&v, 1, line);
    CHECK(line[0] == 0xff, "mode 11 pixel row 0 covers logical rows 0 and 1");
    render_line(&v, 239, line);
    CHECK(line[639 * 3] == 0xff && line[639 * 3 + 2] == 0xff && line[635 * 3] == 0x00,
          "mode 11 last pixel at the bottom-right");
    render_line(&v, 237, line);
    CHECK(line[639 * 3] == 0x00, "mode 11 logical row 237 is pixel row 118");
}

/* The pixel API in mode 11 (160x120), mode 10 (the x > 255 fix) and
 * its refusals in the text modes. */
static const char *PIXEL_LUA =
    "function setup()\n"
    "  assert(ScreenPlot(0, 0, 1) == nil)\n"              /* mode 0: no pixels */
    "  assert(ScreenMode(11) == true)\n"
    "  assert(ScreenPlot(159, 119, 200) == true)\n"
    "  assert(ScreenPlot(160, 0, 1) == nil)\n"            /* 160 wide */
    "  assert(ScreenPlot(0, 120, 1) == nil)\n"            /* 120 high */
    "  assert(ScreenPixelRect(10, 10, 5, 4, 30, true) == true)\n"
    "  assert(ScreenPixelRect(20, 10, 5, 4, 31) == true)\n"
    "  assert(ScreenPixelLine(0, 50, 9, 59, 40) == true)\n"
    "  assert(ScreenPixelCircle(100, 100, 5, 50, true) == true)\n"
    "  assert(ScreenPixelCircle(150, 100, 5, 51) == true)\n"
    "  assert(ScreenBlit(-1, 110, 3, 2, '\\1\\2\\3\\4\\0\\6', 0) == true)\n"
    "  assert(ScreenPixelText(120, 20, 'A', 60, 61) == true)\n"
    "  assert(ScreenPixelText(0, 80, 'A', 62, nil, 2) == true)\n"
    "  assert(ScreenPixelRect(140, 0, 20, 1, 70, true) == true)\n"
    "  assert(ScreenPixelScroll(140, 0, 20, 1, 2, 0, 71) == true)\n"
    "  assert(ScreenBlit(0, 0, 100, 100, string.rep('x', 10000)) == nil)\n"
    "  ExitProgram()\n"
    "end\n"
    "function tick() end\n";

static const char *PIXEL10_LUA =
    "function setup()\n"
    "  assert(ScreenMode(10) == true)\n"
    "  assert(ScreenPlot(300, 239, 9) == true)\n"
    "  ExitProgram()\n"
    "end\n"
    "function tick() end\n";

static void test_pixel_ops(void) {
    video_screens_init();
    mock_set_file("px.lua", PIXEL_LUA);
    CHECK(program_boot("px.lua", NULL), "pixel program boots");
    program_scheduler_step();
    CHECK(program_top() == NULL, "pixel program exited (all asserts held)");
    video_ops_drain();
    const video_state_t *v = video_screen();
    const uint8_t *fb = v->framebuf;
    CHECK(v->mode == VIDEO_MODE_PIXEL_LO && fb != NULL, "mode 11 with a pixel buffer");
#define PX(x, y) (fb[(y) * VIDEO_FB_LO_COLS + (x)])
    CHECK(PX(159, 119) == 200, "plot at the last pixel (160-byte stride)");
    CHECK(PX(10, 10) == 30 && PX(14, 13) == 30 && PX(15, 13) == 0, "filled rect");
    CHECK(PX(20, 10) == 31 && PX(24, 13) == 31 && PX(22, 11) == 0, "outline rect");
    CHECK(PX(0, 50) == 40 && PX(5, 55) == 40 && PX(9, 59) == 40 && PX(1, 50) == 0, "line");
    CHECK(PX(100, 100) == 50 && PX(105, 100) == 50 && PX(100, 106) == 0, "filled circle");
    CHECK(PX(155, 100) == 51 && PX(150, 100) == 0, "circle outline");
    /* Blit at x = -1: column 0 of the sprite is off screen; key 0 skips
     * the fifth pixel (row 1, column 1). */
    CHECK(PX(0, 110) == 2 && PX(1, 110) == 3 && PX(0, 111) == 0 && PX(1, 111) == 6,
          "blit clipped at the left edge with a transparent key");
    /* 'A' row 0 = 0x0C: pixels 2, 3 lit, the rest background. */
    CHECK(PX(122, 20) == 60 && PX(120, 20) == 61 && PX(124, 20) == 61, "pixel text");
    /* At scale 2 the lit pixels 2, 3 become 4..7, two rows deep. */
    CHECK(PX(4, 80) == 62 && PX(7, 81) == 62 && PX(3, 80) == 0 && PX(8, 80) == 0,
          "pixel text at scale 2");
    /* Scroll right 2: 140..141 get the fill, 142..159 the old 70s. */
    CHECK(PX(140, 0) == 71 && PX(141, 0) == 71 && PX(142, 0) == 70 && PX(159, 0) == 70,
          "pixel scroll fills the uncovered pixels");
#undef PX
    video_screens_init();

    mock_set_file("px10.lua", PIXEL10_LUA);
    CHECK(program_boot("px10.lua", NULL), "mode 10 program boots");
    program_scheduler_step();
    video_ops_drain();
    v = video_screen();
    CHECK(v->framebuf && v->framebuf[239 * VIDEO_FB_COLS + 300] == 9 &&
              v->framebuf[239 * VIDEO_FB_COLS + 44] == 0,
          "ScreenPlot x above 255 lands at x, not x & 255");
    video_screens_init();
}

/* ---------------- test 9: Box / Fill (via a program) ---------------- */

static const char *BOX_LUA =
    "function setup()\n"
    "  assert(ScreenMode(1) == true)\n"
    "  assert(ScreenBox(2, 3, 10, 4) == true)\n"
    "  assert(ScreenFill(3, 4, 8, 2, 46, 0x03) == true)\n"
    "  assert(OverlayBox(0, 0, 40, 30, 2, 0x80) == true)\n"
    "  assert(OverlayFill(38, 28, 10, 10, 35) == true)\n"
    "  assert(ScreenBox(0, 0, 1, 5) == nil)\n"
    "  assert(ScreenBox(0, 0, 5, 5, 3) == nil)\n"
    "  assert(ScreenFill(40, 0, 5, 5) == nil)\n"
    "  ExitProgram()\n"
    "end\n"
    "function tick() end\n";

static void test_box_fill(void) {
    video_screens_init();
    mock_set_file("box.lua", BOX_LUA);
    CHECK(program_boot("box.lua", NULL), "box program boots");
    program_scheduler_step();
    CHECK(program_top() == NULL, "box program exited");
    video_ops_drain();

    const video_state_t *v = video_screen();
#define CELL(x, y) ((y) * VIDEO_COLS_2X + (x))
    CHECK(v->base_char[CELL(2, 3)] == 0xDA && v->base_char[CELL(11, 3)] == 0xBF &&
              v->base_char[CELL(2, 6)] == 0xC0 && v->base_char[CELL(11, 6)] == 0xD9,
          "ScreenBox single-line corners");
    CHECK(v->base_char[CELL(5, 3)] == 0xC4 && v->base_char[CELL(5, 6)] == 0xC4 &&
              v->base_char[CELL(2, 5)] == 0xB3 && v->base_char[CELL(11, 4)] == 0xB3,
          "ScreenBox single-line edges");
    CHECK(v->base_char[CELL(3, 4)] == 46 && v->base_attr[CELL(10, 5)] == 0x03,
          "ScreenFill fills the interior with char and attr");
    CHECK(v->base_char[CELL(1, 3)] == ' ' && v->base_char[CELL(12, 6)] == ' ',
          "cells outside the box are untouched");
    CHECK(v->overlay_char[CELL(0, 0)] == 0xC9 && v->overlay_char[CELL(39, 0)] == 0xBB &&
              v->overlay_char[CELL(0, 29)] == 0xC8 &&
              v->overlay_char[CELL(20, 0)] == 0xCD && v->overlay_char[CELL(0, 15)] == 0xBA &&
              v->overlay_attr[CELL(0, 0)] == 0x80,
          "OverlayBox double-line frame at the screen edge");
    /* The fill started at (38,28) with a 10x10 size: only its four
     * on-screen cells are written, and they overwrite the frame corner. */
    CHECK(v->overlay_char[CELL(39, 29)] == 35 && v->overlay_char[CELL(38, 28)] == 35 &&
              v->overlay_attr[CELL(38, 29)] == 0x00 &&
              v->overlay_char[CELL(37, 29)] == 0xCD,
          "OverlayFill is clipped at the screen edge and later cells win");
#undef CELL
    video_screens_init();
}

/* ---------------- test 10: block ops (via a program) ---------------- */

static const char *BLOCK_LUA =
    "function setup()\n"
    "  assert(ScreenMode(1) == true)\n"
    "  assert(ScreenWrite(0, 0, 'HELLO', 0x02) == true)\n"
    "  assert(ScreenWrite(38, 1, 'ABCD') == true)\n"          /* wraps */
    "  assert(ScreenWriteAttr(0, 0, '\\1\\2\\3') == true)\n"
    "  assert(ScreenFill(10, 10, 5, 3, 65, 0x01) == true)\n"
    "  assert(ScreenFillAttr(10, 10, 2, 1, 0x05) == true)\n"
    "  assert(ScreenCopy(10, 10, 5, 3, 20, 20) == true)\n"
    "  assert(ScreenMove(10, 10, 5, 3, 12, 11) == true)\n"     /* overlaps */
    "  assert(ScreenFill(0, 5, 40, 3, 88) == true)\n"           /* 'X' rows */
    "  assert(ScreenScroll(0, 5, 40, 3, -1, 0, 46) == true)\n"  /* left, '.' */
    "  assert(ScreenScroll(0, 5, 40, 3, 0, 1, 45) == true)\n"   /* down, '-' */
    "  assert(OverlayWrite(5, 5, 'OV', 0x80) == true)\n"
    "  assert(OverlayFill(0, 20, 40, 2, 35, 0x03) == true)\n"
    "  assert(OverlayScroll(0, 20, 40, 2, 3, 0, 32, 0x40) == true)\n"
    /* Four writes in a row (the old two-slot staging waited here). */
    "  for i = 1, 4 do assert(ScreenWrite(i - 1, 29, tostring(i)) == true) end\n"
    "  assert(ScreenCopy(0, 0, 5, 5, 40, 0) == nil)\n"
    "  assert(ScreenWrite(40, 0, 'x') == nil)\n"
    "  assert(ScreenScroll(0, 0, 0, 5, 1, 1) == nil)\n"
    "  local ok, err = ScreenLoadImage('x.bmp', 0, 0)\n"
    "  assert(ok == nil and err:find('not available'))\n"
    "  ExitProgram()\n"
    "end\n"
    "function tick() end\n";

static void test_block_ops(void) {
    video_screens_init();
    mock_set_file("blk.lua", BLOCK_LUA);
    CHECK(program_boot("blk.lua", NULL), "block-op program boots");
    program_scheduler_step();
    CHECK(program_top() == NULL, "block-op program exited");
    video_ops_drain();

    const video_state_t *v = video_screen();
#define CELL(x, y) ((y) * VIDEO_COLS_2X + (x))
    CHECK(v->base_char[CELL(0, 0)] == 'H' && v->base_char[CELL(4, 0)] == 'O',
          "ScreenWrite writes the text");
    CHECK(v->base_attr[CELL(0, 0)] == 1 && v->base_attr[CELL(2, 0)] == 3 &&
              v->base_attr[CELL(3, 0)] == 0x02,
          "ScreenWriteAttr overrides attributes cell by cell");
    CHECK(v->base_char[CELL(38, 1)] == 'A' && v->base_char[CELL(39, 1)] == 'B' &&
              v->base_char[CELL(0, 2)] == 'C' && v->base_char[CELL(1, 2)] == 'D',
          "ScreenWrite wraps to the next row");
    CHECK(v->base_char[CELL(20, 20)] == 'A' && v->base_attr[CELL(20, 20)] == 0x05 &&
              v->base_char[CELL(24, 22)] == 'A' && v->base_attr[CELL(24, 22)] == 0x01 &&
              v->base_char[CELL(25, 20)] == ' ',
          "ScreenCopy copies chars and attrs (after FillAttr)");
    CHECK(v->base_char[CELL(10, 10)] == ' ' && v->base_attr[CELL(10, 10)] == 0 &&
              v->base_char[CELL(11, 12)] == ' ',
          "ScreenMove blanks the uncovered part of the source");
    CHECK(v->base_char[CELL(12, 11)] == 'A' && v->base_attr[CELL(12, 11)] == 0x05 &&
              v->base_char[CELL(16, 13)] == 'A' && v->base_char[CELL(12, 12)] == 'A' &&
              v->base_attr[CELL(14, 11)] == 0x01,
          "ScreenMove handles an overlapping destination");
    CHECK(v->base_char[CELL(0, 5)] == '-' && v->base_char[CELL(39, 5)] == '-' &&
              v->base_char[CELL(0, 6)] == 'X' && v->base_char[CELL(39, 6)] == '.' &&
              v->base_char[CELL(39, 7)] == '.' && v->base_char[CELL(0, 8)] == ' ',
          "ScreenScroll shifts and fills, within the region only");
    CHECK(v->overlay_char[CELL(5, 5)] == 'O' && v->overlay_char[CELL(6, 5)] == 'V' &&
              v->overlay_attr[CELL(6, 5)] == 0x80,
          "OverlayWrite writes the overlay");
    CHECK(v->overlay_char[CELL(3, 20)] == 35 && v->overlay_attr[CELL(3, 20)] == 0x03 &&
              v->overlay_char[CELL(0, 20)] == 32 && v->overlay_attr[CELL(0, 21)] == 0x40,
          "OverlayScroll fills uncovered cells with the given attr");
    CHECK(v->base_char[CELL(0, 29)] == '1' && v->base_char[CELL(3, 29)] == '4',
          "consecutive writes each keep their own staging bytes");
#undef CELL
    video_screens_init();
}

/* Text staging: a frame's worth of line writes queues without waiting for
 * core 0, and a ring's worth more still lands intact after it wraps. */
#define STAGING_LUA                                                         \
    "function setup()\n"                                                   \
    "  ScreenMode(1)\n"                                                    \
    "  for y = 0, 29 do ScreenWrite(0, y, string.rep(string.char(65 + y % 26), 40)) end\n" \
    "  ExitProgram()\n"                                                    \
    "end\n"                                                                \
    "function tick() end\n"

#define STAGING_WRAP_LUA                                                    \
    "function setup()\n"                                                   \
    "  ScreenMode(1)\n"                                                    \
    "  for pass = 1, 8 do\n"                                               \
    "    ScreenWrite(0, 0, string.rep(string.char(96 + pass), 1200))\n"    \
    "  end\n"                                                              \
    "  for i = 0, 39 do ScreenWrite(i, 3, string.char(48 + i % 10)) end\n" \
    "  ExitProgram()\n"                                                    \
    "end\n"                                                                \
    "function tick() end\n"

static void test_text_staging(void) {
    video_screens_init();
    mock_set_file("stg.lua", STAGING_LUA);
    g_full_waits = 0;
    CHECK(program_boot("stg.lua", NULL), "staging program boots");
    CHECK(g_full_waits == 0, "30 full-line writes in one frame never wait for core 0");
    video_ops_drain();
    const video_state_t *v = video_screen();
    CHECK(v->base_char[0] == 'A' && v->base_char[39] == 'A' &&
              v->base_char[29 * 40] == 'D' && v->base_char[29 * 40 + 39] == 'D',
          "every line lands from the staging ring");

    /* Eight full screens (9600 bytes) wrap the 8 KB ring: the producer
     * waits for the drain instead of overwriting unapplied text, and the
     * short writes after the wrap are applied intact. */
    video_screens_init();
    mock_set_file("wrap.lua", STAGING_WRAP_LUA);
    g_full_waits = 0;
    CHECK(program_boot("wrap.lua", NULL), "wrapping program boots");
    CHECK(g_full_waits > 0, "a full ring waits for core 0");
    video_ops_drain();
    v = video_screen();
    CHECK(v->base_char[0] == 'h' && v->base_char[1199] == 'h',
          "the last full-screen write wins after the ring wraps");
    CHECK(v->base_char[3 * 40] == '0' && v->base_char[3 * 40 + 9] == '9' &&
              v->base_char[3 * 40 + 39] == '9',
          "short writes after the wrap keep their bytes");
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
    test_render_rom_font();
    test_render_mode80();
    test_mode80_api();
    test_mode80_layout();
    test_render_mode11();
    test_pixel_ops();
    test_box_fill();
    test_block_ops();
    test_text_staging();
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

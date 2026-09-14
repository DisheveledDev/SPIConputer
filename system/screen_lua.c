/* screen_lua.c
 *
 * Display API for programs. Applications never touch display memory:
 * each call appends an op to core 0's change queue (video.h), which is
 * drained at the next frame boundary. The API is synchronous in the
 * sense that a full queue blocks until core 0 has collected the ops, so
 * a Lua program cannot outrun the display.
 *
 *   ScreenMode(mode)                 -> true | nil, err
 *   ScreenOut(x, y, char [, attr])   -> true | nil, err
 *   ScreenAttr(x, y, flags)          -> true | nil, err
 *   ScreenDefineTile(index, bytes)   -> true | nil, err
 *   ScreenPalette(i, r, g, b)        -> true | nil, err
 *   ScreenPaletteSet(t)              -> true | nil, err
 *   ScreenClear([char])              -> true
 *   ScreenBox(x, y, w, h [, style [, attr]])   -> true | nil, err
 *   ScreenFill(x, y, w, h [, char [, attr]])   -> true | nil, err
 *   OverlayOut(x, y, char [, attr])  -> true | nil, err
 *   OverlayAttr(x, y, flags)         -> true | nil, err
 *   OverlayClear([char])             -> true
 *   OverlayBox / OverlayFill         -> as ScreenBox / ScreenFill
 *   ScreenPlot(x, y, colour)         -> true | nil, err (mode 10)
 *
 * Box and Fill are the dialog primitives: Box draws a frame from the
 * ROM font's box-drawing characters (style 1 = single line, 2 =
 * double), Fill writes one character into a rectangle. Both clip to
 * the screen and queue one op per cell.
 *
 * Screen* calls draw on the base layer; Overlay* calls draw on the
 * single overlay layer, whose untouched cells show the base. Mode table:
 * 0 = 40x30 B&W, 1 = 40x30 colour, 10 = 320x240 pixels. The 80-column
 * modes 2 and 3 are retired for now. The RP2040 dev board supports modes
 * 0 and 1 only (no pixel buffer memory there).
 */
#include "screen_lua.h"

#include <stdbool.h>
#include <string.h>

#include "lauxlib.h"

#include "program.h"
#include "video.h"

static void check_program(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "_spi_program");
    program_t *p = (program_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!p || !p->requires_video) {
        luaL_error(L, "Screen API called outside a program");
    }
}

static void put(uint8_t op, int a, int b, int c, uint32_t d, uint32_t e) {
    video_op_t vop = {
        .op = op,
        .a = (uint8_t)a,
        .b = (uint8_t)b,
        .c = (uint8_t)c,
        .d = d,
        .e = e,
    };
    video_op_put(&vop);
}

static int screen_mode(lua_State *L) {
    check_program(L);
    int mode = (int)luaL_checkinteger(L, 1);
#if defined(PICO_RP2040)
    if (mode != VIDEO_MODE_TEXT40 && mode != VIDEO_MODE_TEXT40C) {
        lua_pushnil(L);
        lua_pushliteral(L, "this board supports modes 0 and 1 only");
        return 2;
    }
#endif
    if (!video_mode_valid(mode)) {
        lua_pushnil(L);
        lua_pushfstring(L, "mode %d is not available (0, 1 or 10)", mode);
        return 2;
    }
    video_note_mode(mode);
    put(VIDEO_OP_MODE, mode, 0, 0, 0, 0);
    lua_pushboolean(L, true);
    return 1;
}

static int screen_out(lua_State *L) {
    check_program(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int ch = (int)luaL_checkinteger(L, 3);
    int attr = (int)luaL_optinteger(L, 4, 0);
    if (video_lua_mode() == VIDEO_MODE_PIXEL) {
        return luaL_error(L, "ScreenOut needs a text mode (call ScreenMode first)");
    }
    if (x < 0 || x >= VIDEO_COLS || y < 0 || y >= VIDEO_ROWS ||
        ch < 0 || ch > 255) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    put(VIDEO_OP_OUT, x, y, ch, (uint32_t)(attr & 0xff), 0);
    lua_pushboolean(L, true);
    return 1;
}

static int screen_attr(lua_State *L) {
    check_program(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int flags = (int)luaL_checkinteger(L, 3);
    if (x < 0 || x >= VIDEO_COLS || y < 0 || y >= VIDEO_ROWS) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    put(VIDEO_OP_ATTR, x, y, 0, (uint32_t)(flags & 0xff), 0);
    lua_pushboolean(L, true);
    return 1;
}

static int screen_define_tile(lua_State *L) {
    check_program(L);
    int index = (int)luaL_checkinteger(L, 1);
    if (index < 0 || index > 255) {
        return luaL_error(L, "tile index out of range");
    }
    uint8_t rows[8];
    if (lua_type(L, 2) == LUA_TSTRING) {
        size_t len;
        const char *s = lua_tolstring(L, 2, &len);
        if (len != 8) {
            return luaL_error(L, "tile data must be 8 bytes");
        }
        memcpy(rows, s, 8);
    } else if (lua_istable(L, 2)) {
        for (int i = 0; i < 8; i++) {
            lua_rawgeti(L, 2, i + 1);
            rows[i] = (uint8_t)luaL_checkinteger(L, -1);
            lua_pop(L, 1);
        }
    } else {
        return luaL_error(L, "tile data must be a table or 8-byte string");
    }
    uint32_t lo = (uint32_t)rows[0] | ((uint32_t)rows[1] << 8) |
                  ((uint32_t)rows[2] << 16) | ((uint32_t)rows[3] << 24);
    uint32_t hi = (uint32_t)rows[4] | ((uint32_t)rows[5] << 8) |
                  ((uint32_t)rows[6] << 16) | ((uint32_t)rows[7] << 24);
    put(VIDEO_OP_TILE, index, 0, 0, lo, hi);
    lua_pushboolean(L, true);
    return 1;
}

static int screen_palette(lua_State *L) {
    check_program(L);
    int i = (int)luaL_checkinteger(L, 1);
    int r = (int)luaL_checkinteger(L, 2);
    int g = (int)luaL_checkinteger(L, 3);
    int b = (int)luaL_checkinteger(L, 4);
    if (i < 0 || i > 255) {
        return luaL_error(L, "palette index out of range");
    }
    put(VIDEO_OP_PALETTE, i, 0, 0,
        ((uint32_t)(r & 0xff) << 16) | ((uint32_t)(g & 0xff) << 8) |
            (uint32_t)(b & 0xff),
        0);
    lua_pushboolean(L, true);
    return 1;
}

static int screen_palette_set(lua_State *L) {
    check_program(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    int n = (int)lua_rawlen(L, 1);
    if (n > 256) {
        n = 256;
    }
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 1, i + 1);
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            int r = (int)luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 2);
            int g = (int)luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 3);
            int b = (int)luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            put(VIDEO_OP_PALETTE, i, 0, 0,
                ((uint32_t)(r & 0xff) << 16) | ((uint32_t)(g & 0xff) << 8) |
                    (uint32_t)(b & 0xff),
                0);
        } else {
            int c = (int)luaL_checkinteger(L, -1);
            put(VIDEO_OP_PALETTE, i, 0, 0, (uint32_t)(c & 0xffffff), 0);
        }
        lua_pop(L, 1);
    }
    lua_pushboolean(L, true);
    return 1;
}

static int screen_clear(lua_State *L) {
    check_program(L);
    int ch = (int)luaL_optinteger(L, 1, ' ');
    put(VIDEO_OP_CLEAR, ch & 0xff, 0, 0, 0, 0);
    lua_pushboolean(L, true);
    return 1;
}

static int overlay_out(lua_State *L) {
    check_program(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int ch = (int)luaL_checkinteger(L, 3);
    int attr = (int)luaL_optinteger(L, 4, 0);
    if (video_lua_mode() == VIDEO_MODE_PIXEL) {
        return luaL_error(L, "OverlayOut needs a text mode (call ScreenMode first)");
    }
    if (x < 0 || x >= VIDEO_COLS || y < 0 || y >= VIDEO_ROWS ||
        ch < 0 || ch > 255) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    put(VIDEO_OP_OVER_OUT, x, y, ch, (uint32_t)(attr & 0xff), 0);
    lua_pushboolean(L, true);
    return 1;
}

static int overlay_attr(lua_State *L) {
    check_program(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int flags = (int)luaL_checkinteger(L, 3);
    if (x < 0 || x >= VIDEO_COLS || y < 0 || y >= VIDEO_ROWS) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    put(VIDEO_OP_OVER_ATTR, x, y, 0, (uint32_t)(flags & 0xff), 0);
    lua_pushboolean(L, true);
    return 1;
}

static int overlay_clear(lua_State *L) {
    check_program(L);
    int ch = (int)luaL_optinteger(L, 1, ' ');
    put(VIDEO_OP_OVER_CLEAR, ch & 0xff, 0, 0, 0, 0);
    lua_pushboolean(L, true);
    return 1;
}

/* ---------------- box / fill (dialog primitives) ---------------- */

/* ROM font box-drawing codes (CP437 layout, see font8x8_rom.h):
 * corners top-left, top-right, bottom-left, bottom-right, then the
 * horizontal and vertical line. */
static const uint8_t s_box_single[6] = {0xDA, 0xBF, 0xC0, 0xD9, 0xC4, 0xB3};
static const uint8_t s_box_double[6] = {0xC9, 0xBB, 0xC8, 0xBC, 0xCD, 0xBA};

/* Write one cell to the base or overlay layer, skipping cells that are
 * off the screen (rectangles may be clipped). */
static void put_cell(bool overlay, int x, int y, int ch, int attr) {
    if (x < 0 || x >= VIDEO_COLS || y < 0 || y >= VIDEO_ROWS) {
        return;
    }
    put(overlay ? VIDEO_OP_OVER_OUT : VIDEO_OP_OUT, x, y, ch,
        (uint32_t)(attr & 0xff), 0);
}

/* Shared argument handling for Box and Fill: x, y, w, h; returns false
 * (with nil, err pushed) when the rectangle is empty or entirely off
 * the screen, or the mode has no cells. */
static bool rect_args(lua_State *L, int *x, int *y, int *w, int *h) {
    *x = (int)luaL_checkinteger(L, 1);
    *y = (int)luaL_checkinteger(L, 2);
    *w = (int)luaL_checkinteger(L, 3);
    *h = (int)luaL_checkinteger(L, 4);
    if (video_lua_mode() == VIDEO_MODE_PIXEL) {
        luaL_error(L, "Box/Fill need a text mode (call ScreenMode first)");
    }
    if (*w < 1 || *h < 1 || *x >= VIDEO_COLS || *y >= VIDEO_ROWS ||
        *x + *w <= 0 || *y + *h <= 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return false;
    }
    return true;
}

static int box_common(lua_State *L, bool overlay) {
    check_program(L);
    int x, y, w, h;
    if (!rect_args(L, &x, &y, &w, &h)) {
        return 2;
    }
    int style = (int)luaL_optinteger(L, 5, 1);
    int attr = (int)luaL_optinteger(L, 6, 0);
    if (w < 2 || h < 2) {
        lua_pushnil(L);
        lua_pushliteral(L, "box needs w and h >= 2");
        return 2;
    }
    if (style != 1 && style != 2) {
        lua_pushnil(L);
        lua_pushliteral(L, "style must be 1 (single) or 2 (double)");
        return 2;
    }
    const uint8_t *g = style == 2 ? s_box_double : s_box_single;
    int x1 = x + w - 1;
    int y1 = y + h - 1;
    put_cell(overlay, x, y, g[0], attr);
    put_cell(overlay, x1, y, g[1], attr);
    put_cell(overlay, x, y1, g[2], attr);
    put_cell(overlay, x1, y1, g[3], attr);
    for (int i = x + 1; i < x1; i++) {
        put_cell(overlay, i, y, g[4], attr);
        put_cell(overlay, i, y1, g[4], attr);
    }
    for (int j = y + 1; j < y1; j++) {
        put_cell(overlay, x, j, g[5], attr);
        put_cell(overlay, x1, j, g[5], attr);
    }
    lua_pushboolean(L, true);
    return 1;
}

static int fill_common(lua_State *L, bool overlay) {
    check_program(L);
    int x, y, w, h;
    if (!rect_args(L, &x, &y, &w, &h)) {
        return 2;
    }
    int ch = (int)luaL_optinteger(L, 5, ' ');
    int attr = (int)luaL_optinteger(L, 6, 0);
    if (ch < 0 || ch > 255) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    /* Clip first so a large rectangle costs only its visible cells. */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > VIDEO_COLS ? VIDEO_COLS : x + w;
    int y1 = y + h > VIDEO_ROWS ? VIDEO_ROWS : y + h;
    for (int j = y0; j < y1; j++) {
        for (int i = x0; i < x1; i++) {
            put_cell(overlay, i, j, ch, attr);
        }
    }
    lua_pushboolean(L, true);
    return 1;
}

static int screen_box(lua_State *L) { return box_common(L, false); }
static int screen_fill(lua_State *L) { return fill_common(L, false); }
static int overlay_box(lua_State *L) { return box_common(L, true); }
static int overlay_fill(lua_State *L) { return fill_common(L, true); }

static int screen_plot(lua_State *L) {
    check_program(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int c = (int)luaL_checkinteger(L, 3);
    if (video_lua_mode() != VIDEO_MODE_PIXEL) {
        lua_pushnil(L);
        lua_pushliteral(L, "ScreenPlot needs mode 10");
        return 2;
    }
    if (x < 0 || x >= VIDEO_FB_COLS || y < 0 || y >= VIDEO_FB_ROWS ||
        c < 0 || c > 255) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    put(VIDEO_OP_PLOT, x, y, 0, (uint32_t)c, 0);
    lua_pushboolean(L, true);
    return 1;
}

static const luaL_Reg screen_funcs[] = {
    {"ScreenMode", screen_mode},
    {"ScreenOut", screen_out},
    {"ScreenAttr", screen_attr},
    {"ScreenDefineTile", screen_define_tile},
    {"ScreenPalette", screen_palette},
    {"ScreenPaletteSet", screen_palette_set},
    {"ScreenClear", screen_clear},
    {"ScreenBox", screen_box},
    {"ScreenFill", screen_fill},
    {"OverlayOut", overlay_out},
    {"OverlayAttr", overlay_attr},
    {"OverlayClear", overlay_clear},
    {"OverlayBox", overlay_box},
    {"OverlayFill", overlay_fill},
    {"ScreenPlot", screen_plot},
    {NULL, NULL},
};

void screen_lua_openlibs(lua_State *L) {
    lua_pushglobaltable(L);
    luaL_setfuncs(L, screen_funcs, 0);
    lua_pop(L, 1);
}

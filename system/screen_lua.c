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
 *   ScreenFillAttr(x, y, w, h, attr)           -> true | nil, err
 *   ScreenCopy(sx, sy, w, h, dx, dy)           -> true | nil, err
 *   ScreenMove(sx, sy, w, h, dx, dy [, char [, attr]]) -> true | nil, err
 *   ScreenScroll(x, y, w, h, dx, dy [, char [, attr]]) -> true | nil, err
 *   ScreenWrite(x, y, text [, attr])           -> true | nil, err
 *   ScreenWriteAttr(x, y, attrs)               -> true | nil, err
 *   ScreenLoadImage(path, x, y [, w, h])       -> nil, err (reserved)
 *   OverlayOut(x, y, char [, attr])  -> true | nil, err
 *   OverlayAttr(x, y, flags)         -> true | nil, err
 *   OverlayClear([char])             -> true
 *   Overlay{Box,Fill,FillAttr,Copy,Move,Scroll,Write,WriteAttr}
 *                                    -> as the Screen versions
 *   ScreenPlot(x, y, colour)         -> true | nil, err (mode 10)
 *
 * The block calls (Box, Fill, FillAttr, Copy, Move, Scroll, Write) each
 * queue ONE op, whatever the rectangle's size: core 0 does the work at
 * the frame boundary (video.c). Write carries its bytes through the
 * staging ring. All clip to the screen.
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

/* True while the calling program has been replaced (Launch with
 * replace): the screen slot already belongs to the new program, so the
 * remaining calls of the old program's Lua frame are dropped rather
 * than painted over it. Set by check_program, read by put(). */
static bool s_muted;

static void check_program(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "_spi_program");
    program_t *p = (program_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!p || !p->requires_video) {
        luaL_error(L, "Screen API called outside a program");
    }
    s_muted = p->replaced;
}

static void put(uint8_t op, int a, int b, int c, uint32_t d, uint32_t e) {
    if (s_muted) {
        return;
    }
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

/* ---------------- block ops (one queued op per rectangle) ---------------- */

/* Shared argument handling: x, y, w, h at stack 1..4, clipped to the
 * screen. Returns false (nil, err pushed) for an empty rectangle, one
 * entirely off the screen, or a pixel mode. */
static bool rect_args(lua_State *L, int *x, int *y, int *w, int *h) {
    *x = (int)luaL_checkinteger(L, 1);
    *y = (int)luaL_checkinteger(L, 2);
    *w = (int)luaL_checkinteger(L, 3);
    *h = (int)luaL_checkinteger(L, 4);
    if (video_lua_mode() == VIDEO_MODE_PIXEL) {
        luaL_error(L, "block ops need a text mode (call ScreenMode first)");
    }
    int x1 = *x + *w, y1 = *y + *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (x1 > VIDEO_COLS) x1 = VIDEO_COLS;
    if (y1 > VIDEO_ROWS) y1 = VIDEO_ROWS;
    *w = x1 - *x;
    *h = y1 - *y;
    if (*w < 1 || *h < 1) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return false;
    }
    return true;
}

static bool cell_arg(lua_State *L, int idx, int *x, int *y) {
    *x = (int)luaL_checkinteger(L, idx);
    *y = (int)luaL_checkinteger(L, idx + 1);
    return *x >= 0 && *x < VIDEO_COLS && *y >= 0 && *y < VIDEO_ROWS;
}

static int push_true(lua_State *L) {
    lua_pushboolean(L, true);
    return 1;
}

/* Box(x, y, w, h [, style [, attr]]): a frame from the ROM font's
 * box-drawing glyphs, style 1 single line (default) or 2 double. */
static int box_common(lua_State *L, bool overlay) {
    check_program(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    int style = (int)luaL_optinteger(L, 5, 1);
    int attr = (int)luaL_optinteger(L, 6, 0);
    if (video_lua_mode() == VIDEO_MODE_PIXEL) {
        return luaL_error(L, "block ops need a text mode (call ScreenMode first)");
    }
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
    if (x < 0 || y < 0 || x >= VIDEO_COLS || y >= VIDEO_ROWS) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    if (w > 255) w = 255;
    if (h > 255) h = 255;
    uint32_t flags = overlay ? VIDEO_BLK_OVERLAY : 0;
    put(VIDEO_OP_BOX, x, y, w,
        (uint32_t)h | ((uint32_t)style << 8) | ((uint32_t)(attr & 0xff) << 16) |
            (flags << 24),
        0);
    return push_true(L);
}

/* Fill(x, y, w, h [, char [, attr]]): one character and attribute into
 * every cell of the rectangle. */
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
    uint32_t flags = (overlay ? VIDEO_BLK_OVERLAY : 0) | VIDEO_BLK_CHARS |
                     VIDEO_BLK_ATTRS;
    put(VIDEO_OP_RECT, x, y, w,
        (uint32_t)h | ((uint32_t)ch << 8) | ((uint32_t)(attr & 0xff) << 16) |
            (flags << 24),
        0);
    return push_true(L);
}

/* FillAttr(x, y, w, h, attr): attributes only; characters stay. */
static int fill_attr_common(lua_State *L, bool overlay) {
    check_program(L);
    int x, y, w, h;
    if (!rect_args(L, &x, &y, &w, &h)) {
        return 2;
    }
    int attr = (int)luaL_checkinteger(L, 5);
    uint32_t flags = (overlay ? VIDEO_BLK_OVERLAY : 0) | VIDEO_BLK_ATTRS;
    put(VIDEO_OP_RECT, x, y, w,
        (uint32_t)h | ((uint32_t)(attr & 0xff) << 16) | (flags << 24), 0);
    return push_true(L);
}

/* Copy(sx, sy, w, h, dx, dy) and Move(sx, sy, w, h, dx, dy [, char
 * [, attr]]): a block to a new top-left corner; Move blanks what the
 * block uncovers. Both rectangles must start on the screen; the size
 * is clipped so both fit. */
static int copy_common(lua_State *L, bool overlay, bool move) {
    check_program(L);
    int x, y, w, h;
    if (!rect_args(L, &x, &y, &w, &h)) {
        return 2;
    }
    int dx, dy;
    if (!cell_arg(L, 5, &dx, &dy)) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    int ch = (int)luaL_optinteger(L, 7, ' ');
    int attr = (int)luaL_optinteger(L, 8, 0);
    uint32_t flags = (overlay ? VIDEO_BLK_OVERLAY : 0) | (move ? VIDEO_BLK_CLEAR : 0);
    put(VIDEO_OP_COPY, x, y, w,
        (uint32_t)h | ((uint32_t)dx << 8) | ((uint32_t)dy << 16) | (flags << 24),
        (uint32_t)(ch & 0xff) | ((uint32_t)(attr & 0xff) << 8));
    return push_true(L);
}

/* Scroll(x, y, w, h, dx, dy [, char [, attr]]): shift a region's
 * contents by (dx, dy) cells; uncovered cells are filled. */
static int scroll_common(lua_State *L, bool overlay) {
    check_program(L);
    int x, y, w, h;
    if (!rect_args(L, &x, &y, &w, &h)) {
        return 2;
    }
    int dx = (int)luaL_checkinteger(L, 5);
    int dy = (int)luaL_checkinteger(L, 6);
    int ch = (int)luaL_optinteger(L, 7, ' ');
    int attr = (int)luaL_optinteger(L, 8, 0);
    if (dx < -127) dx = -127;
    if (dx > 127) dx = 127;
    if (dy < -127) dy = -127;
    if (dy > 127) dy = 127;
    uint32_t flags = overlay ? VIDEO_BLK_OVERLAY : 0;
    put(VIDEO_OP_SCROLL, x, y, w,
        (uint32_t)h | (((uint32_t)dx & 0xffu) << 8) | (((uint32_t)dy & 0xffu) << 16) |
            (flags << 24),
        (uint32_t)(ch & 0xff) | ((uint32_t)(attr & 0xff) << 8));
    return push_true(L);
}

/* Write(x, y, text [, attr]): the bytes of `text` as consecutive cells
 * from (x, y), wrapping to the next row; with `attr` every cell gets
 * that attribute too. One op via the staging ring, however long the text
 * (clipped at the end of the screen). */
static int write_common(lua_State *L, bool overlay, bool attrs_only) {
    check_program(L);
    int x, y;
    if (video_lua_mode() == VIDEO_MODE_PIXEL) {
        return luaL_error(L, "block ops need a text mode (call ScreenMode first)");
    }
    if (!cell_arg(L, 1, &x, &y)) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    size_t len = 0;
    const char *text = luaL_checklstring(L, 3, &len);
    bool set_attr = !attrs_only && !lua_isnoneornil(L, 4);
    int attr = set_attr ? (int)luaL_checkinteger(L, 4) : 0;
    size_t room = (size_t)(VIDEO_COLS * VIDEO_ROWS - (y * VIDEO_COLS + x));
    if (len > room) len = room;
    if (len == 0 || s_muted) {
        return push_true(L);
    }
    uint32_t end;
    uint8_t *dst = video_staging_acquire((uint32_t)len, &end);
    memcpy(dst, text, len);
    uint32_t flags = (overlay ? VIDEO_BLK_OVERLAY : 0) |
                     (attrs_only ? VIDEO_BLK_BYTES_ATTR : 0) |
                     (set_attr ? VIDEO_BLK_SET_ATTR : 0);
    video_op_t op = {
        .op = VIDEO_OP_TEXT,
        .a = (uint8_t)x,
        .b = (uint8_t)y,
        .c = 0,
        .d = (uint32_t)len | ((uint32_t)(attr & 0xff) << 16) | (flags << 24),
        .e = end,
    };
    video_op_put(&op);
    return push_true(L);
}

static int screen_box(lua_State *L) { return box_common(L, false); }
static int screen_fill(lua_State *L) { return fill_common(L, false); }
static int screen_fill_attr(lua_State *L) { return fill_attr_common(L, false); }
static int screen_copy(lua_State *L) { return copy_common(L, false, false); }
static int screen_move(lua_State *L) { return copy_common(L, false, true); }
static int screen_scroll(lua_State *L) { return scroll_common(L, false); }
static int screen_write(lua_State *L) { return write_common(L, false, false); }
static int screen_write_attr(lua_State *L) { return write_common(L, false, true); }
static int overlay_box(lua_State *L) { return box_common(L, true); }
static int overlay_fill(lua_State *L) { return fill_common(L, true); }
static int overlay_fill_attr(lua_State *L) { return fill_attr_common(L, true); }
static int overlay_copy(lua_State *L) { return copy_common(L, true, false); }
static int overlay_move(lua_State *L) { return copy_common(L, true, true); }
static int overlay_scroll(lua_State *L) { return scroll_common(L, true); }
static int overlay_write(lua_State *L) { return write_common(L, true, false); }
static int overlay_write_attr(lua_State *L) { return write_common(L, true, true); }

/* ScreenLoadImage(path, x, y [, w, h]): reserved. The interface exists
 * so frameworks can offer Screen.LoadImage today; the loader (BMP into
 * tiles or the mode 10 buffer) is not implemented yet. */
static int screen_load_image(lua_State *L) {
    check_program(L);
    luaL_checkstring(L, 1);
    luaL_checkinteger(L, 2);
    luaL_checkinteger(L, 3);
    lua_pushnil(L);
    lua_pushliteral(L, "ScreenLoadImage is not available yet");
    return 2;
}

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
    {"ScreenFillAttr", screen_fill_attr},
    {"ScreenCopy", screen_copy},
    {"ScreenMove", screen_move},
    {"ScreenScroll", screen_scroll},
    {"ScreenWrite", screen_write},
    {"ScreenWriteAttr", screen_write_attr},
    {"ScreenLoadImage", screen_load_image},
    {"OverlayOut", overlay_out},
    {"OverlayAttr", overlay_attr},
    {"OverlayClear", overlay_clear},
    {"OverlayBox", overlay_box},
    {"OverlayFill", overlay_fill},
    {"OverlayFillAttr", overlay_fill_attr},
    {"OverlayCopy", overlay_copy},
    {"OverlayMove", overlay_move},
    {"OverlayScroll", overlay_scroll},
    {"OverlayWrite", overlay_write},
    {"OverlayWriteAttr", overlay_write_attr},
    {"ScreenPlot", screen_plot},
    {NULL, NULL},
};

void screen_lua_openlibs(lua_State *L) {
    lua_pushglobaltable(L);
    luaL_setfuncs(L, screen_funcs, 0);
    lua_pop(L, 1);
}

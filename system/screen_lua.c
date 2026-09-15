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
 *   ScreenPlot(x, y, colour)         -> true | nil, err (modes 10, 11)
 *   ScreenPixelRect(x, y, w, h, colour [, filled])      -> true | nil, err
 *   ScreenPixelLine(x0, y0, x1, y1, colour)             -> true | nil, err
 *   ScreenPixelCircle(cx, cy, r, colour [, filled])     -> true | nil, err
 *   ScreenPixelScroll(x, y, w, h, dx, dy [, fill])      -> true | nil, err
 *   ScreenBlit(x, y, w, h, pixels [, key])              -> true | nil, err
 *   ScreenPixelText(x, y, text, colour [, bg [, scale]]) -> true | nil, err
 *
 * The pixel calls draw on the pixel buffer of mode 10 (320x240) or mode
 * 11 (160x120). Each is one queued op; core 0 clips, so shapes and
 * sprites may run off the edges. ScreenBlit and ScreenPixelText carry
 * their bytes through the staging ring (a blit is at most 8184 pixels;
 * the Graphics framework splits larger images).
 *
 * The block calls (Box, Fill, FillAttr, Copy, Move, Scroll, Write) each
 * queue ONE op, whatever the rectangle's size: core 0 does the work at
 * the frame boundary (video.c). Write carries its bytes through the
 * staging ring. All clip to the screen.
 *
 * Screen* calls draw on the base layer; Overlay* calls draw on the
 * single overlay layer, whose untouched cells show the base. Mode table:
 * 0 = 40x30 B&W, 1 = 40x30 colour, 2 = 80x60 B&W, 3 = 80x60 colour,
 * 10 = 320x240 pixels, 11 = 160x120 pixels. Cell and pixel coordinates
 * are checked against the mode the program last selected. The RP2040 dev board supports modes 0 and 1
 * only (no pixel buffer memory there).
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

/* The text geometry of the mode the program last selected. */
static int lua_cols(void) { return video_mode_cols(video_lua_mode()); }
static int lua_rows(void) { return video_mode_rows(video_lua_mode()); }

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
        lua_pushfstring(L, "mode %d is not available (0-3, 10 or 11)", mode);
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
    if (video_mode_has_pixels(video_lua_mode())) {
        return luaL_error(L, "ScreenOut needs a text mode (call ScreenMode first)");
    }
    if (x < 0 || x >= lua_cols() || y < 0 || y >= lua_rows() ||
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
    if (x < 0 || x >= lua_cols() || y < 0 || y >= lua_rows()) {
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
    if (video_mode_has_pixels(video_lua_mode())) {
        return luaL_error(L, "OverlayOut needs a text mode (call ScreenMode first)");
    }
    if (x < 0 || x >= lua_cols() || y < 0 || y >= lua_rows() ||
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
    if (x < 0 || x >= lua_cols() || y < 0 || y >= lua_rows()) {
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
    if (video_mode_has_pixels(video_lua_mode())) {
        luaL_error(L, "block ops need a text mode (call ScreenMode first)");
    }
    int x1 = *x + *w, y1 = *y + *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (x1 > lua_cols()) x1 = lua_cols();
    if (y1 > lua_rows()) y1 = lua_rows();
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
    return *x >= 0 && *x < lua_cols() && *y >= 0 && *y < lua_rows();
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
    if (video_mode_has_pixels(video_lua_mode())) {
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
    if (x < 0 || y < 0 || x >= lua_cols() || y >= lua_rows()) {
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
    if (video_mode_has_pixels(video_lua_mode())) {
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
    size_t room = (size_t)(lua_cols() * lua_rows() - (y * lua_cols() + x));
    if (len > room) len = room;
    if (len > VIDEO_STAGING_BYTES) len = VIDEO_STAGING_BYTES; /* never waits forever */
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

/* ---------------- pixel ops (modes 4 and 10) ---------------- */

/* Pushes nil, err and returns false unless a pixel mode is selected. */
static bool pixel_mode(lua_State *L) {
    if (video_mode_has_pixels(video_lua_mode())) {
        return true;
    }
    lua_pushnil(L);
    lua_pushliteral(L, "needs a pixel mode (ScreenMode 10 or 11)");
    return false;
}

/* A coordinate as the int16 the op carries. */
static int coord_arg(lua_State *L, int idx) {
    lua_Integer v = luaL_checkinteger(L, idx);
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    return (int)v;
}

static uint32_t pack16(int lo, int hi) {
    return (uint32_t)(uint16_t)(int16_t)lo | ((uint32_t)(uint16_t)(int16_t)hi << 16);
}

static int colour_arg(lua_State *L, int idx) {
    return (int)(luaL_checkinteger(L, idx) & 0xff);
}

static int screen_plot(lua_State *L) {
    check_program(L);
    int x = coord_arg(L, 1);
    int y = coord_arg(L, 2);
    int c = colour_arg(L, 3);
    if (!pixel_mode(L)) {
        return 2;
    }
    int mode = video_lua_mode();
    if (x < 0 || x >= video_pixel_width(mode) || y < 0 || y >= video_pixel_height(mode)) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    put(VIDEO_OP_PLOT, c, 0, 0, pack16(x, y), 0);
    return push_true(L);
}

static int screen_pixel_rect(lua_State *L) {
    check_program(L);
    int x = coord_arg(L, 1), y = coord_arg(L, 2);
    int w = coord_arg(L, 3), h = coord_arg(L, 4);
    int c = colour_arg(L, 5);
    int filled = lua_toboolean(L, 6) ? VIDEO_PX_FILLED : 0;
    if (!pixel_mode(L)) {
        return 2;
    }
    put(VIDEO_OP_PRECT, c, filled, 0, pack16(x, y), pack16(w, h));
    return push_true(L);
}

static int screen_pixel_line(lua_State *L) {
    check_program(L);
    int x0 = coord_arg(L, 1), y0 = coord_arg(L, 2);
    int x1 = coord_arg(L, 3), y1 = coord_arg(L, 4);
    int c = colour_arg(L, 5);
    if (!pixel_mode(L)) {
        return 2;
    }
    put(VIDEO_OP_PLINE, c, 0, 0, pack16(x0, y0), pack16(x1, y1));
    return push_true(L);
}

static int screen_pixel_circle(lua_State *L) {
    check_program(L);
    int cx = coord_arg(L, 1), cy = coord_arg(L, 2);
    lua_Integer r = luaL_checkinteger(L, 3);
    int c = colour_arg(L, 4);
    int filled = lua_toboolean(L, 5) ? VIDEO_PX_FILLED : 0;
    if (!pixel_mode(L)) {
        return 2;
    }
    if (r < 0 || r > 1024) {
        lua_pushnil(L);
        lua_pushliteral(L, "radius out of range (0-1024)");
        return 2;
    }
    put(VIDEO_OP_PCIRCLE, c, filled, 0, pack16(cx, cy), (uint32_t)r);
    return push_true(L);
}

static int screen_pixel_scroll(lua_State *L) {
    check_program(L);
    int x = coord_arg(L, 1), y = coord_arg(L, 2);
    int w = coord_arg(L, 3), h = coord_arg(L, 4);
    lua_Integer dx = luaL_checkinteger(L, 5), dy = luaL_checkinteger(L, 6);
    int fill = (int)(luaL_optinteger(L, 7, 0) & 0xff);
    if (!pixel_mode(L)) {
        return 2;
    }
    if (dx < -127) dx = -127;
    if (dx > 127) dx = 127;
    if (dy < -127) dy = -127;
    if (dy > 127) dy = 127;
    put(VIDEO_OP_PSCROLL, fill, (int)dx & 0xff, (int)dy & 0xff, pack16(x, y), pack16(w, h));
    return push_true(L);
}

/* Queue a staged pixel op: `header` (int16 LE values) then `body`. */
static void put_staged(uint8_t kind, int a, int b, int c, const int16_t *header,
                       int header_count, const char *body, size_t body_len) {
    if (s_muted) {
        return;
    }
    uint32_t len = (uint32_t)(header_count * 2) + (uint32_t)body_len;
    uint32_t end;
    uint8_t *dst = video_staging_acquire(len, &end);
    for (int i = 0; i < header_count; i++) {
        dst[i * 2] = (uint8_t)((uint16_t)header[i] & 0xff);
        dst[i * 2 + 1] = (uint8_t)((uint16_t)header[i] >> 8);
    }
    memcpy(dst + header_count * 2, body, body_len);
    video_op_t op = {
        .op = kind, .a = (uint8_t)a, .b = (uint8_t)b, .c = (uint8_t)c,
        .d = len, .e = end,
    };
    video_op_put(&op);
}

/* ScreenBlit(x, y, w, h, pixels [, key]): w*h palette bytes, row by
 * row; with `key` that colour is transparent (sprites). */
static int screen_blit(lua_State *L) {
    check_program(L);
    int x = coord_arg(L, 1), y = coord_arg(L, 2);
    lua_Integer w = luaL_checkinteger(L, 3), h = luaL_checkinteger(L, 4);
    size_t len = 0;
    const char *pixels = luaL_checklstring(L, 5, &len);
    bool keyed = !lua_isnoneornil(L, 6);
    int key = keyed ? colour_arg(L, 6) : 0;
    if (!pixel_mode(L)) {
        return 2;
    }
    if (w < 1 || h < 1 || w > VIDEO_FB_COLS || h > VIDEO_FB_ROWS) {
        lua_pushnil(L);
        lua_pushliteral(L, "size out of range");
        return 2;
    }
    size_t count = (size_t)(w * h);
    if (count + 8 > VIDEO_STAGING_BYTES) {
        lua_pushnil(L);
        lua_pushfstring(L, "blit too large (%d pixels at most)", (int)VIDEO_STAGING_BYTES - 8);
        return 2;
    }
    if (len < count) {
        lua_pushnil(L);
        lua_pushfstring(L, "pixels: %d bytes for %dx%d", (int)len, (int)w, (int)h);
        return 2;
    }
    int16_t header[4] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
    put_staged(VIDEO_OP_BLIT, key, keyed ? VIDEO_PX_KEYED : 0, 0, header, 4, pixels, count);
    return push_true(L);
}

/* ScreenPixelText(x, y, text, colour [, bg [, scale]]): 8x8 glyphs (the
 * font or the program's tiles) drawn into the pixel buffer, each glyph
 * pixel scale x scale (1-4); with `bg` the glyph cells are painted,
 * otherwise only the lit pixels are. */
static int screen_pixel_text(lua_State *L) {
    check_program(L);
    int x = coord_arg(L, 1), y = coord_arg(L, 2);
    size_t len = 0;
    const char *text = luaL_checklstring(L, 3, &len);
    int c = colour_arg(L, 4);
    bool filled = !lua_isnoneornil(L, 5);
    int bg = filled ? colour_arg(L, 5) : 0;
    lua_Integer scale = luaL_optinteger(L, 6, 1);
    if (!pixel_mode(L)) {
        return 2;
    }
    if (scale < 1 || scale > 4) {
        lua_pushnil(L);
        lua_pushliteral(L, "scale must be 1-4");
        return 2;
    }
    if (len > 64) len = 64; /* 512 pixels: past the edge anyway */
    if (len == 0) {
        return push_true(L);
    }
    int flags = (filled ? VIDEO_PX_FILLED : 0) | (((int)scale - 1) << VIDEO_PX_SCALE_SHIFT);
    int16_t header[2] = {(int16_t)x, (int16_t)y};
    put_staged(VIDEO_OP_PTEXT, c, flags, bg, header, 2, text, len);
    return push_true(L);
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
    {"ScreenPixelRect", screen_pixel_rect},
    {"ScreenPixelLine", screen_pixel_line},
    {"ScreenPixelCircle", screen_pixel_circle},
    {"ScreenPixelScroll", screen_pixel_scroll},
    {"ScreenBlit", screen_blit},
    {"ScreenPixelText", screen_pixel_text},
    {NULL, NULL},
};

void screen_lua_openlibs(lua_State *L) {
    lua_pushglobaltable(L);
    luaL_setfuncs(L, screen_funcs, 0);
    lua_pop(L, 1);
}

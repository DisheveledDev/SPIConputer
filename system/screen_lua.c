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
 *   OverlayOut(x, y, char [, attr])  -> true | nil, err
 *   OverlayAttr(x, y, flags)         -> true | nil, err
 *   OverlayClear([char])             -> true
 *   ScreenPlot(x, y, colour)         -> true | nil, err (mode 10)
 *
 * Screen* calls draw on the base layer; Overlay* calls draw on the
 * single overlay layer, whose untouched cells show the base. Mode table:
 * 0 = 40x30 B&W, 1 = 40x30 colour, 10 = 320x240 pixels. The 80-column
 * modes 2 and 3 are retired for now. The RP2040 dev board supports modes
 * 0 and 1 only (no pixel buffer memory there).
 */
#include "screen_lua.h"

#include <stdio.h>
#include <string.h>

#include "lauxlib.h"

#include "fs_core0.h"
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
    char detail[32];
    snprintf(detail, sizeof(detail), "screen_mode=%d", mode);
    fs_core0_debug_log(detail);
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
    {"OverlayOut", overlay_out},
    {"OverlayAttr", overlay_attr},
    {"OverlayClear", overlay_clear},
    {"ScreenPlot", screen_plot},
    {NULL, NULL},
};

void screen_lua_openlibs(lua_State *L) {
    lua_pushglobaltable(L);
    luaL_setfuncs(L, screen_funcs, 0);
    lua_pop(L, 1);
}

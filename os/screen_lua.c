/* screen_lua.c
 *
 * Display API for programs (Phase 1). All functions mutate the current
 * program's video state (g_current_video) and bump its version, which
 * the core 0 serial mirror watches. The renderer (render.c) reads the
 * same state.
 *
 *   ScreenMode(mode)                 -> true | nil, err
 *   ScreenOut(x, y, char [, attr])   -> true | nil, err
 *   ScreenAttr(x, y, flags)          -> true | nil, err
 *   ScreenDefineTile(index, bytes)   -> true | nil, err
 *   ScreenPalette(i, r, g, b)        -> true | nil, err
 *   ScreenPaletteSet(t)              -> true | nil, err
 *   ScreenClear([char])              -> true
 *   ScreenPlot(x, y, colour)         -> true | nil, err (mode 10)
 *
 * Mode table: 0 = 40x30 B&W, 1 = 40x30 colour, 2 = 80x60 B&W,
 * 3 = 80x60 colour, 10 = 320x240 pixels. The RP2040 dev board
 * supports modes 0 and 1 only (no framebuffer memory policy there).
 */
#include "screen_lua.h"

#include <string.h>

#include "lauxlib.h"

#include "video.h"

static video_state_t *current(lua_State *L) {
    if (!g_current_video) {
        luaL_error(L, "Screen API called outside a program");
    }
    return g_current_video;
}

static int screen_mode(lua_State *L) {
    video_state_t *v = current(L);
    int mode = (int)luaL_checkinteger(L, 1);
#if defined(PICO_RP2040)
    if (mode != VIDEO_MODE_TEXT40 && mode != VIDEO_MODE_TEXT40C) {
        lua_pushnil(L);
        lua_pushliteral(L, "this board supports modes 0 and 1 only");
        return 2;
    }
#endif
    if (!video_set_mode(v, mode)) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot set mode %d (out of memory?)", mode);
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int screen_out(lua_State *L) {
    video_state_t *v = current(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int ch = (int)luaL_checkinteger(L, 3);
    int attr = (int)luaL_optinteger(L, 4, 0);
    int cols = video_char_cols(v);
    int rows = video_char_rows(v);
    if (v->mode == VIDEO_MODE_PIXEL) {
        return luaL_error(L, "ScreenOut needs a text mode (call ScreenMode first)");
    }
    if (x < 0 || x >= cols || y < 0 || y >= rows || ch < 0 || ch > 255) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    v->char_map[y * cols + x] = (uint8_t)ch;
    v->attr_map[y * cols + x] = (uint8_t)attr;
    v->version++;
    lua_pushboolean(L, true);
    return 1;
}

static int screen_attr(lua_State *L) {
    video_state_t *v = current(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int flags = (int)luaL_checkinteger(L, 3);
    int cols = video_char_cols(v);
    int rows = video_char_rows(v);
    if (x < 0 || x >= cols || y < 0 || y >= rows) {
        lua_pushnil(L);
        lua_pushliteral(L, "out of range");
        return 2;
    }
    v->attr_map[y * cols + x] = (uint8_t)flags;
    v->version++;
    lua_pushboolean(L, true);
    return 1;
}

static int screen_define_tile(lua_State *L) {
    video_state_t *v = current(L);
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
    memcpy(v->tiles[index], rows, 8);
    v->tile_defined[index] = 1;
    v->version++;
    lua_pushboolean(L, true);
    return 1;
}

static int screen_palette(lua_State *L) {
    video_state_t *v = current(L);
    int i = (int)luaL_checkinteger(L, 1);
    int r = (int)luaL_checkinteger(L, 2);
    int g = (int)luaL_checkinteger(L, 3);
    int b = (int)luaL_checkinteger(L, 4);
    if (i < 0 || i > 255) {
        return luaL_error(L, "palette index out of range");
    }
    v->palette[i] = ((uint32_t)(r & 0xff) << 16) | ((uint32_t)(g & 0xff) << 8) |
                    (uint32_t)(b & 0xff);
    v->version++;
    lua_pushboolean(L, true);
    return 1;
}

static int screen_palette_set(lua_State *L) {
    video_state_t *v = current(L);
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
            v->palette[i] = ((uint32_t)(r & 0xff) << 16) |
                            ((uint32_t)(g & 0xff) << 8) | (uint32_t)(b & 0xff);
        } else {
            int c = (int)luaL_checkinteger(L, -1);
            v->palette[i] = (uint32_t)(c & 0xffffff);
        }
        lua_pop(L, 1);
    }
    v->version++;
    lua_pushboolean(L, true);
    return 1;
}

static int screen_clear(lua_State *L) {
    video_state_t *v = current(L);
    int ch = (int)luaL_optinteger(L, 1, ' ');
    if (v->mode == VIDEO_MODE_PIXEL) {
        memset(v->framebuf, (uint8_t)ch, VIDEO_FB_COLS * VIDEO_FB_ROWS);
    } else {
        memset(v->char_map, (uint8_t)ch, video_char_cols(v) * video_char_rows(v));
        memset(v->attr_map, 0, video_char_cols(v) * video_char_rows(v));
    }
    v->version++;
    lua_pushboolean(L, true);
    return 1;
}

static int screen_plot(lua_State *L) {
    video_state_t *v = current(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int c = (int)luaL_checkinteger(L, 3);
    if (v->mode != VIDEO_MODE_PIXEL) {
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
    v->framebuf[y * VIDEO_FB_COLS + x] = (uint8_t)c;
    v->version++;
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
    {"ScreenPlot", screen_plot},
    {NULL, NULL},
};

void screen_lua_openlibs(lua_State *L) {
    lua_pushglobaltable(L);
    luaL_setfuncs(L, screen_funcs, 0);
    lua_pop(L, 1);
}

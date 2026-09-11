/* screen_lua.h — display module for programs (Phase 1) */
#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the display globals: ScreenMode, ScreenOut, ScreenAttr,
 * ScreenDefineTile, ScreenPalette, ScreenPaletteSet, ScreenClear,
 * ScreenPlot. Requires a running program (g_current_video). */
void screen_lua_openlibs(lua_State *L);

#ifdef __cplusplus
}
#endif

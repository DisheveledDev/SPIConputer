/* screen_lua.h — display module for programs (Phase 1) */
#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the display globals: ScreenMode, ScreenOut, ScreenAttr,
 * ScreenDefineTile, ScreenPalette, ScreenPaletteSet, ScreenClear,
 * ScreenPlot. Requires a running program; calls queue ops for core 0 to
 * apply (see video.h). */
void screen_lua_openlibs(lua_State *L);

#ifdef __cplusplus
}
#endif

/* sound_lua.h — sound/music module for programs (Phase 8) */
#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the sound globals: SoundDefine, SoundLoad, SoundPlay,
 * SoundStop, SoundStopAll, SoundVolume, MusicDefine, MusicPlay,
 * MusicStop, MusicPlaying. Requires a running program (g_current_audio). */
void sound_lua_openlibs(lua_State *L);

#ifdef __cplusplus
}
#endif

/* sys_lua.h — OS services for programs (Phase 5) */
#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the OS globals: TimeNow, Pid, ExitProgram, Launch,
 * TimerCreate, TimerStop, InputPoll, InputControl. */
void sys_lua_openlibs(lua_State *L);

#ifdef __cplusplus
}
#endif

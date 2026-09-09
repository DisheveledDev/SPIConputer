/* fatfs_lua.h
 *
 * Bridge between the Lua interpreter and the FatFs SD card driver.
 */
#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the SD card (logical drive "0:"). Returns true on success. */
bool fatfs_lua_mount(void);

/* True if the SD card is currently mounted. */
bool fatfs_lua_mounted(void);

/* Open the standard Lua libraries plus the SD-backed "fs" module.
 * Replaces the global loadfile/dofile with SD-card versions and installs
 * an SD-card searcher for require(). */
void fatfs_lua_openlibs(lua_State *L);

/* Load and run a Lua script from the SD card. Returns 0 on success,
 * non-zero on failure (error printed to stderr). */
int fatfs_lua_run_file(lua_State *L, const char *path);

#ifdef __cplusplus
}
#endif

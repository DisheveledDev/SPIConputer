/* fs_lua.h
 *
 * Core 1 Lua <-> filesystem bridge. All FatFs work happens on core 0
 * behind the RPC (fs_core0.c); this module only sends RPC requests, so
 * core 1 never touches the SD hardware.
 */
#pragma once

#include "ff.h" /* FRESULT */
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open the standard Lua libraries plus the SD-backed "fs" module.
 * Replaces the global loadfile/dofile with SD-card versions and installs
 * an SD-card searcher for require(). */
void fs_lua_openlibs(lua_State *L);

/* Load and run a Lua script from the SD card. Returns 0 on success,
 * non-zero on failure (error printed to stderr). */
int fs_lua_run_file(lua_State *L, const char *path);

/* Whole-file read over the RPC (capped at MAX_SCRIPT_SIZE). Returns
 * FR_OK and a malloc'd buffer, or a FRESULT code on failure. */
FRESULT fs_lua_readall(const char *path, char **out, size_t *out_len);

/* Chunked read helpers (streaming callers, e.g. SoundLoad). Each read
 * returns up to RPC_STAGING_SIZE bytes; a short/zero read means EOF. */
FRESULT fs_lua_open_read(const char *path, int32_t *handle);
FRESULT fs_lua_read_chunk(int32_t handle, void *dst, size_t max, size_t *got);
FRESULT fs_lua_close_handle(int32_t handle);

#ifdef __cplusplus
}
#endif

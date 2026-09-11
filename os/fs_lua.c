/* fs_lua.c
 *
 * Lua <-> filesystem bridge for the SPIComputer (core 1 side).
 *
 * Exposes an "fs" Lua module backed by the SD card (FAT16/FAT32/exFAT),
 * with every operation carried over the core 1 -> core 0 RPC:
 *
 *   fs.open(path [, mode])   -> file object or nil, err
 *   fs.ls([path])            -> array of {name=, size=, dir=}
 *   fs.stat(path)            -> {size=, dir=} or nil, err
 *   fs.exists(path)          -> bool
 *   fs.mkdir(path)           -> true or nil, err
 *   fs.remove(path)          -> true or nil, err
 *   fs.rename(old, new)      -> true or nil, err
 *   fs.free()                -> free_kb, total_kb
 *   fs.ready()               -> bool (SD mounted)
 *   fs.mount()               -> bool
 *   fs.readall(path)         -> string or nil, err
 *   fs.writeall(path, data)  -> true or nil, err
 *
 * File objects (same functions also usable as methods):
 *
 *   f:read(n)                -> string (may be shorter than n at EOF)
 *   f:write(s)               -> bytes written
 *   f:seek(ofs [, "set"|"cur"|"end"]) -> new position
 *   f:tell()                 -> position
 *   f:size()                 -> file size
 *   f:flush()                -> sync
 *   f:close()
 *
 * File objects hold a core 0 handle id; the FIL objects themselves stay
 * on core 0 (fs_core0.c owns the handle pool).
 *
 * The globals loadfile/dofile and require()'s file searcher are replaced
 * with SD-card backed versions, so "dofile('os.lua')" reads from FAT
 * (via RPC, compiled on core 1).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "ff.h"      /* FA_* flags and FRESULT codes (headers only) */
#include "f_util.h"  /* FRESULT_str */

#include "rpc.h"

#define FS_FILE_MT "FS_FILE"

/* Cap script size read into RAM (RP2350 has 520 KB total). */
#define MAX_SCRIPT_SIZE (128u * 1024u)

/* ------------------------------------------------------------------ */
/* RPC plumbing (core 1 side)                                          */
/* ------------------------------------------------------------------ */

static int rpc_fs(rpc_op_t op, int32_t a, int32_t b, int32_t whence,
                  const char *path1, const char *path2,
                  rpc_response_t *resp) {
    rpc_request_t req;
    memset(&req, 0, sizeof(req));
    req.op = op;
    req.a = a;
    req.b = b;
    req.whence = whence;
    if (path1) {
        snprintf(req.path1, sizeof(req.path1), "%s", path1);
    }
    if (path2) {
        snprintf(req.path2, sizeof(req.path2), "%s", path2);
    }
    return rpc_call(&req, resp);
}

/* Whole-file read into a malloc'd buffer (NULL on failure). */
FRESULT fs_lua_readall(const char *path, char **out, size_t *out_len) {
    rpc_response_t resp;
    int32_t handle;
    char *buf = NULL;
    size_t used = 0, cap = 0;
    FRESULT err = FR_OK;

    rpc_fs(RPC_FS_OPEN, FA_READ, 0, 0, path, NULL, &resp);
    if (resp.result != FR_OK) {
        return (FRESULT)resp.result;
    }
    handle = resp.value;

    for (;;) {
        rpc_fs(RPC_FS_READ, handle, RPC_STAGING_SIZE, 0, NULL, NULL, &resp);
        if (resp.result != FR_OK) {
            err = (FRESULT)resp.result;
            break;
        }
        if (resp.value == 0) {
            break; /* EOF */
        }
        if (used + (size_t)resp.value > MAX_SCRIPT_SIZE) {
            err = FR_DENIED; /* too large */
            break;
        }
        if (used + (size_t)resp.value > cap) {
            size_t ncap = cap ? cap * 2 : 256;
            while (ncap < used + (size_t)resp.value) {
                ncap *= 2;
            }
            char *nbuf = (char *)realloc(buf, ncap);
            if (!nbuf) {
                err = FR_NOT_ENOUGH_CORE;
                break;
            }
            buf = nbuf;
            cap = ncap;
        }
        memcpy(buf + used, rpc_staging(), (size_t)resp.value);
        used += (size_t)resp.value;
    }
    rpc_fs(RPC_FS_CLOSE, handle, 0, 0, NULL, NULL, &resp);
    if (err != FR_OK) {
        free(buf);
        return err;
    }
    *out = buf ? buf : (char *)calloc(1, 1);
    *out_len = used;
    return FR_OK;
}

/* Chunked read helpers for callers that stream a file (SoundLoad): open
 * a read handle, pull up to RPC_STAGING_SIZE bytes at a time, close. */
FRESULT fs_lua_open_read(const char *path, int32_t *handle) {
    rpc_response_t resp;
    rpc_fs(RPC_FS_OPEN, FA_READ, 0, 0, path, NULL, &resp);
    if (resp.result != FR_OK) {
        return (FRESULT)resp.result;
    }
    *handle = resp.value;
    return FR_OK;
}

FRESULT fs_lua_read_chunk(int32_t handle, void *dst, size_t max, size_t *got) {
    rpc_response_t resp;
    if (max > RPC_STAGING_SIZE) {
        max = RPC_STAGING_SIZE;
    }
    rpc_fs(RPC_FS_READ, handle, (int32_t)max, 0, NULL, NULL, &resp);
    if (resp.result != FR_OK) {
        *got = 0;
        return (FRESULT)resp.result;
    }
    size_t n = (size_t)resp.value > max ? max : (size_t)resp.value;
    memcpy(dst, rpc_staging(), n);
    *got = n;
    return FR_OK;
}

FRESULT fs_lua_close_handle(int32_t handle) {
    rpc_response_t resp;
    rpc_fs(RPC_FS_CLOSE, handle, 0, 0, NULL, NULL, &resp);
    return (FRESULT)resp.result;
}

/* ------------------------------------------------------------------ */
/* File objects                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t handle; /* 0 = closed */
} FsFile;

static FsFile *check_file(lua_State *L, int idx) {
    return (FsFile *)luaL_checkudata(L, idx, FS_FILE_MT);
}

static int file_gc(lua_State *L) {
    FsFile *f = (FsFile *)luaL_checkudata(L, 1, FS_FILE_MT);
    if (f->handle != 0) {
        rpc_response_t resp;
        rpc_fs(RPC_FS_CLOSE, f->handle, 0, 0, NULL, NULL, &resp);
        f->handle = 0;
    }
    return 0;
}

static int fs_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *mode = luaL_optstring(L, 2, "r");
    BYTE flags = 0;
    for (const char *m = mode; *m; m++) {
        switch (*m) {
            case 'r': flags |= FA_READ; break;
            case 'w': flags |= FA_CREATE_ALWAYS | FA_WRITE; break;
            case 'a': flags |= FA_OPEN_APPEND | FA_WRITE; break;
            case '+': flags |= FA_READ | FA_WRITE; break;
        }
    }
    if (!(flags & (FA_READ | FA_WRITE))) flags |= FA_READ;

    rpc_response_t resp;
    rpc_fs(RPC_FS_OPEN, flags, 0, 0, path, NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open '%s': %s", path,
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }

    FsFile *f = (FsFile *)lua_newuserdatauv(L, sizeof(FsFile), 0);
    f->handle = resp.value;
    luaL_setmetatable(L, FS_FILE_MT);
    return 1;
}

static int file_read(lua_State *L) {
    FsFile *f = check_file(L, 1);
    lua_Integer n = luaL_checkinteger(L, 2);
    if (n <= 0) return luaL_error(L, "read size must be > 0");
    if ((uint64_t)n > MAX_SCRIPT_SIZE) return luaL_error(L, "read size too large");
    char *buf = (char *)malloc((size_t)n);
    if (!buf) return luaL_error(L, "out of memory");

    size_t total = 0;
    while (total < (size_t)n) {
        size_t chunk = (size_t)n - total;
        if (chunk > RPC_STAGING_SIZE) chunk = RPC_STAGING_SIZE;
        rpc_response_t resp;
        rpc_fs(RPC_FS_READ, f->handle, (int32_t)chunk, 0, NULL, NULL, &resp);
        if (resp.result != FR_OK) {
            free(buf);
            lua_pushnil(L);
            lua_pushfstring(L, "read failed: %s",
                            FRESULT_str((FRESULT)resp.result));
            return 2;
        }
        memcpy(buf + total, rpc_staging(), (size_t)resp.value);
        total += (size_t)resp.value;
        if ((size_t)resp.value < chunk) {
            break; /* EOF */
        }
    }
    lua_pushlstring(L, buf, total);
    free(buf);
    return 1;
}

static int file_write(lua_State *L) {
    FsFile *f = check_file(L, 1);
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);

    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > RPC_STAGING_SIZE) chunk = RPC_STAGING_SIZE;
        memcpy(rpc_staging(), s + total, chunk);
        rpc_response_t resp;
        rpc_fs(RPC_FS_WRITE, f->handle, (int32_t)chunk, 0, NULL, NULL, &resp);
        if (resp.result != FR_OK) {
            lua_pushnil(L);
            lua_pushfstring(L, "write failed: %s",
                            FRESULT_str((FRESULT)resp.result));
            return 2;
        }
        total += (size_t)resp.value;
        if ((size_t)resp.value < chunk) {
            break; /* card full */
        }
    }
    lua_pushinteger(L, (lua_Integer)total);
    return 1;
}

static int file_seek(lua_State *L) {
    FsFile *f = check_file(L, 1);
    lua_Integer ofs = luaL_checkinteger(L, 2);
    const char *w = luaL_optstring(L, 3, "set");
    int32_t whence;
    if (strcmp(w, "cur") == 0) {
        whence = 1;
    } else if (strcmp(w, "end") == 0) {
        whence = 2;
    } else if (strcmp(w, "set") == 0) {
        whence = 0;
    } else {
        return luaL_error(L, "invalid seek mode '%s'", w);
    }
    rpc_response_t resp;
    rpc_fs(RPC_FS_SEEK, f->handle, (int32_t)ofs, whence, NULL, NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "seek failed: %s",
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)resp.value);
    return 1;
}

static int file_tell(lua_State *L) {
    FsFile *f = check_file(L, 1);
    rpc_response_t resp;
    rpc_fs(RPC_FS_TELL, f->handle, 0, 0, NULL, NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "tell failed: %s",
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)resp.value);
    return 1;
}

static int file_size(lua_State *L) {
    FsFile *f = check_file(L, 1);
    rpc_response_t resp;
    rpc_fs(RPC_FS_SIZE, f->handle, 0, 0, NULL, NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "size failed: %s",
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)resp.value);
    return 1;
}

static int file_flush(lua_State *L) {
    FsFile *f = check_file(L, 1);
    rpc_response_t resp;
    rpc_fs(RPC_FS_FLUSH, f->handle, 0, 0, NULL, NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "sync failed: %s",
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int file_close(lua_State *L) {
    FsFile *f = check_file(L, 1);
    if (f->handle != 0) {
        rpc_response_t resp;
        rpc_fs(RPC_FS_CLOSE, f->handle, 0, 0, NULL, NULL, &resp);
        f->handle = 0;
        if (resp.result != FR_OK) {
            lua_pushnil(L);
            lua_pushfstring(L, "close failed: %s",
                            FRESULT_str((FRESULT)resp.result));
            return 2;
        }
    }
    lua_pushboolean(L, true);
    return 1;
}

/* ------------------------------------------------------------------ */
/* fs module functions                                                 */
/* ------------------------------------------------------------------ */

static int fs_ls(lua_State *L) {
    const char *path = luaL_optstring(L, 1, "");
    rpc_response_t resp;
    rpc_fs(RPC_FS_LS, 0, 0, 0, path, NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open directory '%s': %s", path,
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }

    lua_newtable(L);
    const uint8_t *p = rpc_staging();
    int i = 1;
    for (int32_t n = 0; n < resp.value; n++) {
        const char *name = (const char *)p;
        size_t nlen = strlen(name);
        p += nlen + 1;
        uint32_t sz;
        memcpy(&sz, p, 4);
        p += 4;
        uint8_t is_dir = *p++;
        lua_newtable(L);
        lua_pushstring(L, name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)sz);
        lua_setfield(L, -2, "size");
        lua_pushboolean(L, is_dir != 0);
        lua_setfield(L, -2, "dir");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int fs_stat(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    rpc_response_t resp;
    rpc_fs(RPC_FS_STAT, 0, 0, 0, path, NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "no such file '%s'", path);
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)resp.value);
    lua_setfield(L, -2, "size");
    lua_pushboolean(L, resp.value2 != 0);
    lua_setfield(L, -2, "dir");
    return 1;
}

static int fs_exists(lua_State *L) {
    rpc_response_t resp;
    rpc_fs(RPC_FS_EXISTS, 0, 0, 0, luaL_checkstring(L, 1), NULL, &resp);
    lua_pushboolean(L, resp.value != 0);
    return 1;
}

static int fs_mkdir(lua_State *L) {
    rpc_response_t resp;
    rpc_fs(RPC_FS_MKDIR, 0, 0, 0, luaL_checkstring(L, 1), NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "mkdir failed: %s",
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int fs_remove(lua_State *L) {
    rpc_response_t resp;
    rpc_fs(RPC_FS_REMOVE, 0, 0, 0, luaL_checkstring(L, 1), NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "remove failed: %s",
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int fs_rename(lua_State *L) {
    const char *oldp = luaL_checkstring(L, 1);
    const char *newp = luaL_checkstring(L, 2);
    rpc_response_t resp;
    rpc_fs(RPC_FS_RENAME, 0, 0, 0, oldp, newp, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "rename failed: %s",
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int fs_free(lua_State *L) {
    rpc_response_t resp;
    rpc_fs(RPC_FS_FREE, 0, 0, 0, NULL, NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "f_getfree failed: %s",
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)resp.value);
    lua_pushinteger(L, (lua_Integer)resp.value2);
    return 2;
}

static int fs_ready(lua_State *L) {
    rpc_response_t resp;
    rpc_fs(RPC_FS_READY, 0, 0, 0, NULL, NULL, &resp);
    lua_pushboolean(L, resp.value != 0);
    return 1;
}

static int fs_mount(lua_State *L) {
    rpc_response_t resp;
    rpc_fs(RPC_FS_MOUNT, 0, 0, 0, NULL, NULL, &resp);
    lua_pushboolean(L, resp.value != 0);
    return 1;
}

static int fs_readall(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len = 0;
    char *buf = NULL;
    FRESULT err = fs_lua_readall(path, &buf, &len);
    if (err != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot read '%s': %s", path, FRESULT_str(err));
        return 2;
    }
    lua_pushlstring(L, buf, len);
    free(buf);
    return 1;
}

static int fs_writeall(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    rpc_response_t resp;
    rpc_fs(RPC_FS_OPEN, FA_CREATE_ALWAYS | FA_WRITE, 0, 0, path, NULL, &resp);
    if (resp.result != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open '%s': %s", path,
                        FRESULT_str((FRESULT)resp.result));
        return 2;
    }
    int32_t handle = resp.value;

    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > RPC_STAGING_SIZE) chunk = RPC_STAGING_SIZE;
        memcpy(rpc_staging(), data + total, chunk);
        rpc_fs(RPC_FS_WRITE, handle, (int32_t)chunk, 0, NULL, NULL, &resp);
        if (resp.result != FR_OK) {
            rpc_fs(RPC_FS_CLOSE, handle, 0, 0, NULL, NULL, &resp);
            lua_pushnil(L);
            lua_pushfstring(L, "write failed: %s",
                            FRESULT_str((FRESULT)resp.result));
            return 2;
        }
        total += (size_t)resp.value;
        if ((size_t)resp.value < chunk) {
            break;
        }
    }
    rpc_fs(RPC_FS_CLOSE, handle, 0, 0, NULL, NULL, &resp);
    lua_pushboolean(L, true);
    return 1;
}

static const luaL_Reg fs_funcs[] = {
    {"open", fs_open},
    {"read", file_read},
    {"write", file_write},
    {"seek", file_seek},
    {"tell", file_tell},
    {"size", file_size},
    {"close", file_close},
    {"flush", file_flush},
    {"ls", fs_ls},
    {"stat", fs_stat},
    {"exists", fs_exists},
    {"mkdir", fs_mkdir},
    {"remove", fs_remove},
    {"rename", fs_rename},
    {"free", fs_free},
    {"ready", fs_ready},
    {"mount", fs_mount},
    {"readall", fs_readall},
    {"writeall", fs_writeall},
    {NULL, NULL},
};

static const luaL_Reg file_methods[] = {
    {"read", file_read},
    {"write", file_write},
    {"seek", file_seek},
    {"tell", file_tell},
    {"size", file_size},
    {"close", file_close},
    {"flush", file_flush},
    {NULL, NULL},
};

int luaopen_fs(lua_State *L) {
    lua_newtable(L);
    luaL_setfuncs(L, fs_funcs, 0);
    if (luaL_newmetatable(L, FS_FILE_MT)) {
        lua_newtable(L);
        luaL_setfuncs(L, file_methods, 0);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, file_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* SD-backed loadfile/dofile/require                                   */
/* ------------------------------------------------------------------ */

/* Pushes the loaded chunk (1 result) or nil + error message (2 results). */
static int loadfile_from_sd(lua_State *L, const char *path) {
    size_t len = 0;
    char *buf = NULL;
    FRESULT err = fs_lua_readall(path, &buf, &len);
    if (err != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open '%s': %s", path, FRESULT_str(err));
        return 2;
    }
    int st = luaL_loadbufferx(L, buf, len, path, "t");
    free(buf);
    if (st != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    return 1;
}

static int g_loadfile(lua_State *L) {
    return loadfile_from_sd(L, luaL_checkstring(L, 1));
}

static int g_dofile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int n = loadfile_from_sd(L, path);
    if (!lua_isfunction(L, -1)) {
        lua_remove(L, -2); /* drop nil, keep error message */
        return lua_error(L);
    }
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - 1;
}

/* Searcher for require(): looks for NAME, NAME.lua, /NAME, /NAME.lua,
 * /lib/NAME(.lua) on the SD card. Returns a loader function or an error
 * message (exactly one result, as the require() protocol expects). */
static int searcher_from_sd(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    char mod[240];
    size_t j = 0;
    for (size_t i = 0; name[i] && j < sizeof(mod) - 1; i++)
        mod[j++] = (name[i] == '.') ? '/' : name[i];
    mod[j] = '\0';

    const char *fmts[] = {"%s", "%s.lua", "/%s", "/%s.lua", "/lib/%s", "/lib/%s.lua"};
    for (size_t k = 0; k < sizeof(fmts) / sizeof(fmts[0]); k++) {
        char path[260];
        snprintf(path, sizeof(path), fmts[k], mod);
        rpc_response_t resp;
        rpc_fs(RPC_FS_EXISTS, 0, 0, 0, path, NULL, &resp);
        if (resp.value != 0) {
            int n = loadfile_from_sd(L, path);
            if (lua_isfunction(L, -1)) return 1;
            lua_remove(L, -2); /* drop nil, keep error message */
            return 1;
        }
    }
    lua_pushfstring(L, "\n\tno file for module '%s' on the SD card", name);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Boot helper                                                         */
/* ------------------------------------------------------------------ */

void fs_lua_openlibs(lua_State *L) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_LOADLIBNAME, luaopen_package, 1);
    lua_pop(L, 1);
    luaL_requiref(L, "fs", luaopen_fs, 1);
    lua_pop(L, 1);

    /* SD-backed loadfile/dofile */
    lua_pushcfunction(L, g_loadfile);
    lua_setglobal(L, "loadfile");
    lua_pushcfunction(L, g_dofile);
    lua_setglobal(L, "dofile");

    /* SD-backed file searcher for require() (slot 2, after preload) */
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "searchers");
    lua_pushcfunction(L, searcher_from_sd);
    lua_rawseti(L, -2, 2);
    lua_pop(L, 2);
}

int fs_lua_run_file(lua_State *L, const char *path) {
    int n = loadfile_from_sd(L, path);
    if (!lua_isfunction(L, -1)) {
        fprintf(stderr, "boot: %s\n", lua_tostring(L, -1));
        lua_pop(L, n);
        return 1;
    }
    int st = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (st != LUA_OK) {
        fprintf(stderr, "boot: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 1;
    }
    lua_pop(L, lua_gettop(L));
    return 0;
}

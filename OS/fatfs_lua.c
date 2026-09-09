/* fatfs_lua.c
 *
 * Lua <-> FatFs bridge for the SPIComputer.
 *
 * Exposes an "fs" Lua module backed by the SD card (FAT16/FAT32/exFAT):
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
 * The globals loadfile/dofile and require()'s file searcher are replaced
 * with SD-card backed versions, so "dofile('os.lua')" reads from FAT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "ff.h"
#include "f_util.h"
#include "hw_config.h"

#define FATFS_FILE_MT "FATFS_FILE"

/* Cap script size read into RAM (RP2350 has 520 KB total). */
#define MAX_SCRIPT_SIZE (128u * 1024u)

static bool g_mounted = false;

/* ------------------------------------------------------------------ */
/* SD card mount                                                       */
/* ------------------------------------------------------------------ */

bool fatfs_lua_mount(void) {
    sd_card_t *pSD = sd_get_by_num(0);
    if (!pSD) return false;
    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    g_mounted = (fr == FR_OK);
    return g_mounted;
}

bool fatfs_lua_mounted(void) {
    return g_mounted;
}

/* ------------------------------------------------------------------ */
/* File objects                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    FIL fil;
} FatFsFile;

static FatFsFile *check_file(lua_State *L, int idx) {
    return (FatFsFile *)luaL_checkudata(L, idx, FATFS_FILE_MT);
}

static int file_gc(lua_State *L) {
    FatFsFile *f = (FatFsFile *)luaL_checkudata(L, 1, FATFS_FILE_MT);
    if (f->fil.obj.fs) f_close(&f->fil);
    f->fil.obj.fs = NULL;
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

    FatFsFile *f = (FatFsFile *)lua_newuserdatauv(L, sizeof(FatFsFile), 0);
    memset(f, 0, sizeof(*f));
    FRESULT fr = f_open(&f->fil, path, flags);
    if (fr != FR_OK) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open '%s': %s", path, FRESULT_str(fr));
        return 2;
    }
    luaL_setmetatable(L, FATFS_FILE_MT);
    return 1;
}

static int file_read(lua_State *L) {
    FatFsFile *f = check_file(L, 1);
    lua_Integer n = luaL_checkinteger(L, 2);
    if (n <= 0) return luaL_error(L, "read size must be > 0");
    if ((uint64_t)n > MAX_SCRIPT_SIZE) return luaL_error(L, "read size too large");
    char *buf = (char *)malloc((size_t)n);
    if (!buf) return luaL_error(L, "out of memory");
    UINT got = 0;
    FRESULT fr = f_read(&f->fil, buf, (UINT)n, &got);
    if (fr != FR_OK) {
        free(buf);
        lua_pushnil(L);
        lua_pushfstring(L, "read failed: %s", FRESULT_str(fr));
        return 2;
    }
    lua_pushlstring(L, buf, got);
    free(buf);
    return 1;
}

static int file_write(lua_State *L) {
    FatFsFile *f = check_file(L, 1);
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);
    UINT written = 0;
    FRESULT fr = f_write(&f->fil, s, (UINT)len, &written);
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "write failed: %s", FRESULT_str(fr));
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)written);
    return 1;
}

static int file_seek(lua_State *L) {
    FatFsFile *f = check_file(L, 1);
    lua_Integer ofs = luaL_checkinteger(L, 2);
    const char *w = luaL_optstring(L, 3, "set");
    FSIZE_t base = 0;
    if (strcmp(w, "cur") == 0) {
        base = f_tell(&f->fil);
    } else if (strcmp(w, "end") == 0) {
        base = f_size(&f->fil);
    } else if (strcmp(w, "set") != 0) {
        return luaL_error(L, "invalid seek mode '%s'", w);
    }
    FRESULT fr = f_lseek(&f->fil, base + (FSIZE_t)ofs);
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "seek failed: %s", FRESULT_str(fr));
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)f_tell(&f->fil));
    return 1;
}

static int file_tell(lua_State *L) {
    FatFsFile *f = check_file(L, 1);
    lua_pushinteger(L, (lua_Integer)f_tell(&f->fil));
    return 1;
}

static int file_size(lua_State *L) {
    FatFsFile *f = check_file(L, 1);
    lua_pushinteger(L, (lua_Integer)f_size(&f->fil));
    return 1;
}

static int file_flush(lua_State *L) {
    FatFsFile *f = check_file(L, 1);
    FRESULT fr = f_sync(&f->fil);
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "sync failed: %s", FRESULT_str(fr));
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int file_close(lua_State *L) {
    FatFsFile *f = check_file(L, 1);
    FRESULT fr = FR_OK;
    if (f->fil.obj.fs) fr = f_close(&f->fil);
    f->fil.obj.fs = NULL;
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "close failed: %s", FRESULT_str(fr));
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

/* ------------------------------------------------------------------ */
/* fs module functions                                                 */
/* ------------------------------------------------------------------ */

static int fs_ls(lua_State *L) {
    const char *path = luaL_optstring(L, 1, "");
    DIR dj;
    FILINFO fno;
    FRESULT fr = f_opendir(&dj, path);
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open directory '%s': %s", path, FRESULT_str(fr));
        return 2;
    }
    lua_newtable(L);
    int i = 1;
    for (;;) {
        fr = f_readdir(&dj, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) break;
        lua_newtable(L);
        lua_pushstring(L, fno.fname);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)fno.fsize);
        lua_setfield(L, -2, "size");
        lua_pushboolean(L, (fno.fattrib & AM_DIR) != 0);
        lua_setfield(L, -2, "dir");
        lua_rawseti(L, -2, i++);
    }
    f_closedir(&dj);
    return 1;
}

static int fs_stat(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "no such file '%s'", path);
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)fno.fsize);
    lua_setfield(L, -2, "size");
    lua_pushboolean(L, (fno.fattrib & AM_DIR) != 0);
    lua_setfield(L, -2, "dir");
    return 1;
}

static int fs_exists(lua_State *L) {
    FILINFO fno;
    lua_pushboolean(L, f_stat(luaL_checkstring(L, 1), &fno) == FR_OK);
    return 1;
}

static int fs_mkdir(lua_State *L) {
    FRESULT fr = f_mkdir(luaL_checkstring(L, 1));
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "mkdir failed: %s", FRESULT_str(fr));
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int fs_remove(lua_State *L) {
    FRESULT fr = f_unlink(luaL_checkstring(L, 1));
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "remove failed: %s", FRESULT_str(fr));
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int fs_rename(lua_State *L) {
    const char *oldp = luaL_checkstring(L, 1);
    const char *newp = luaL_checkstring(L, 2);
    FRESULT fr = f_rename(oldp, newp);
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "rename failed: %s", FRESULT_str(fr));
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int fs_free(lua_State *L) {
    FATFS *fs;
    DWORD fre_clst = 0;
    FRESULT fr = f_getfree("0:", &fre_clst, &fs);
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "f_getfree failed: %s", FRESULT_str(fr));
        return 2;
    }
    uint64_t free_kb = (uint64_t)fre_clst * fs->csize / 2;
    uint64_t total_kb = (uint64_t)(fs->n_fatent - 2) * fs->csize / 2;
    lua_pushinteger(L, (lua_Integer)free_kb);
    lua_pushinteger(L, (lua_Integer)total_kb);
    return 2;
}

static int fs_ready(lua_State *L) {
    lua_pushboolean(L, g_mounted);
    return 1;
}

static int fs_mount(lua_State *L) {
    lua_pushboolean(L, fatfs_lua_mount());
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
    if (luaL_newmetatable(L, FATFS_FILE_MT)) {
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
    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open '%s': %s", path, FRESULT_str(fr));
        return 2;
    }
    FSIZE_t sz = f_size(&fil);
    if (sz > MAX_SCRIPT_SIZE) {
        f_close(&fil);
        lua_pushnil(L);
        lua_pushfstring(L, "'%s' is too large to load (%u bytes)", path, (unsigned)sz);
        return 2;
    }
    char *buf = (char *)malloc(sz ? (size_t)sz : 1);
    if (!buf) {
        f_close(&fil);
        lua_pushnil(L);
        lua_pushliteral(L, "out of memory");
        return 2;
    }
    UINT got = 0;
    fr = f_read(&fil, buf, (UINT)sz, &got);
    f_close(&fil);
    if (fr != FR_OK) {
        free(buf);
        lua_pushnil(L);
        lua_pushfstring(L, "read failed: %s", FRESULT_str(fr));
        return 2;
    }
    int st = luaL_loadbufferx(L, buf, got, path, "t");
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
        FILINFO fno;
        if (f_stat(path, &fno) == FR_OK && !(fno.fattrib & AM_DIR)) {
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

void fatfs_lua_openlibs(lua_State *L) {
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

int fatfs_lua_run_file(lua_State *L, const char *path) {
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

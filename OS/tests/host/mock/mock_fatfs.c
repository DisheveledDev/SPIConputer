/* mock/mock_fatfs.c
 *
 * In-memory mock of the FatFs + SD card layer for host-side testing.
 * Serves a small virtual disk:
 *   - "os.lua"      -> script content supplied by the test at startup
 *   - "/lib/hello.lua" -> "print('hello module loaded from SD')\nreturn 'hello-ok'\n"
 *   - "boot.log"    -> writable RAM file (256 bytes max)
 *   - "lib"         -> directory
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "f_util.h"
#include "hw_config.h"

static const char *g_os_lua = NULL;
static size_t g_os_lua_len = 0;
static char g_boot_log[256];
static size_t g_boot_log_len = 0;
static FATFS g_fatfs = { .csize = 64, .n_fatent = 2000 };
static sd_card_t g_sd = { .pcName = "0:" };

sd_card_t *sd_get_by_num(size_t num) { return num == 0 ? &g_sd : NULL; }

void mock_set_os_lua(const char *content, size_t len) {
    g_os_lua = content;
    g_os_lua_len = len;
}

const char *mock_boot_log(void) { return g_boot_log; }
size_t mock_boot_log_len(void) { return g_boot_log_len; }

FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt) {
    (void)path; (void)opt;
    memcpy(fs, &g_fatfs, sizeof(FATFS));
    return FR_OK;
}

static FRESULT resolve(const char *path, const char **content, size_t *len, int *is_dir) {
    *is_dir = 0;
    if (strcmp(path, "os.lua") == 0 || strcmp(path, "/os.lua") == 0) {
        *content = g_os_lua; *len = g_os_lua_len; return FR_OK;
    }
    if (strcmp(path, "/lib/hello.lua") == 0 || strcmp(path, "lib/hello.lua") == 0) {
        *content = "print('hello module loaded from SD')\nreturn 'hello-ok'\n";
        *len = strlen(*content); return FR_OK;
    }
    if (strcmp(path, "boot.log") == 0) { *content = NULL; *len = 0; return FR_OK; }
    if (strcmp(path, "lib") == 0 || strcmp(path, "/lib") == 0) { *is_dir = 1; return FR_OK; }
    return FR_NO_FILE;
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode) {
    const char *content; size_t len; int is_dir;
    FRESULT r = resolve(path, &content, &len, &is_dir);
    if (r != FR_OK || is_dir) return r == FR_OK ? FR_DENIED : r;
    memset(fp, 0, sizeof(*fp));
    fp->obj.fs = &g_fatfs;
    if (strcmp(path, "boot.log") == 0) {
        fp->buf = (BYTE *)g_boot_log;
        if (mode & FA_CREATE_ALWAYS) g_boot_log_len = 0;
        fp->len = g_boot_log_len;
    } else {
        fp->buf = (BYTE *)content;
        fp->len = len;
    }
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
    size_t avail = fp->len - (size_t)fp->fptr;
    size_t n = btr < avail ? btr : avail;
    memcpy(buff, fp->buf + fp->fptr, n);
    fp->fptr += n;
    *br = (UINT)n;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
    size_t n = btw;
    if (fp->len + n > sizeof(g_boot_log)) n = sizeof(g_boot_log) - fp->len;
    memcpy(fp->buf + fp->len, buff, n);
    fp->len += n;
    g_boot_log_len = fp->len;
    *bw = (UINT)n;
    return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
    if (ofs > fp->len) return FR_DENIED;
    fp->fptr = ofs;
    return FR_OK;
}

FSIZE_t f_tell(FIL *fp) { return fp->fptr; }
FSIZE_t f_size(FIL *fp) { return fp->len; }
FRESULT f_sync(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_close(FIL *fp) { fp->obj.fs = NULL; return FR_OK; }

FRESULT f_opendir(DIR *dp, const TCHAR *path) {
    (void)path;
    dp->idx = 0;
    return FR_OK;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno) {
    static const struct { const char *name; int dir; } entries[] = {
        { "os.lua", 0 }, { "lib", 1 }, { "boot.log", 0 },
    };
    if (dp->idx >= 3) { fno->fname[0] = 0; return FR_OK; }
    memset(fno, 0, sizeof(*fno));
    strcpy(fno->fname, entries[dp->idx].name);
    if (!entries[dp->idx].dir) {
        fno->fsize = (dp->idx == 0) ? g_os_lua_len : g_boot_log_len;
    }
    fno->fattrib = entries[dp->idx].dir ? AM_DIR : 0;
    dp->idx++;
    return FR_OK;
}

FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }

FRESULT f_stat(const TCHAR *path, FILINFO *fno) {
    const char *content; size_t len; int is_dir;
    FRESULT r = resolve(path, &content, &len, &is_dir);
    if (r != FR_OK) return r;
    memset(fno, 0, sizeof(*fno));
    fno->fsize = is_dir ? 0 : len;
    fno->fattrib = is_dir ? AM_DIR : 0;
    return FR_OK;
}

FRESULT f_mkdir(const TCHAR *path) { (void)path; return FR_OK; }
FRESULT f_unlink(const TCHAR *path) { (void)path; return FR_OK; }
FRESULT f_rename(const TCHAR *o, const TCHAR *n) { (void)o; (void)n; return FR_OK; }

FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs) {
    (void)path;
    *nclst = 100;
    *fatfs = &g_fatfs;
    return FR_OK;
}

DWORD get_fattime(void) { return 0; }

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
#include "sd_card.h"

bool sd_init_driver(void) { return true; }

/* Simulated card removal/reinsertion (Phase 4 acceptance test). */
static int g_ejected = 0;
void mock_sd_eject(void) { g_ejected = 1; }
void mock_sd_insert(void) { g_ejected = 0; }

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

/* Arbitrary program files for the process-model tests. Writable: the
 * mock keeps an owned buffer per entry so tests can read back what a
 * program saved. */
#define MOCK_FILES_MAX 16
#define MOCK_FILE_CAP 4096
typedef struct {
    char *path;
    char *data;
    size_t len;
    size_t cap;
} mock_file_t;
static mock_file_t g_files[MOCK_FILES_MAX];

void mock_set_file_bytes(const char *path, const void *data, size_t len);

void mock_set_file(const char *path, const char *content) {
    mock_set_file_bytes(path, content, strlen(content));
}

/* Binary-safe variant (WAV files etc.). Files larger than the write
 * cap are allowed (read-only use); the buffer is sized accordingly. */
void mock_set_file_bytes(const char *path, const void *data, size_t len) {
    size_t cap = len + 1 > MOCK_FILE_CAP ? len + 1 : (size_t)MOCK_FILE_CAP;
    for (int i = 0; i < MOCK_FILES_MAX; i++) {
        if (g_files[i].path == NULL || strcmp(g_files[i].path, path) == 0) {
            free(g_files[i].path);
            free(g_files[i].data);
            g_files[i].path = strdup(path);
            g_files[i].data = malloc(cap);
            g_files[i].len = len;
            g_files[i].cap = cap;
            memcpy(g_files[i].data, data, len);
            g_files[i].data[len] = '\0';
            return;
        }
    }
}

const char *mock_get_file(const char *path) {
    for (int i = 0; i < MOCK_FILES_MAX; i++) {
        if (g_files[i].path && strcmp(g_files[i].path, path) == 0) {
            return g_files[i].data;
        }
    }
    return NULL;
}

static int g_file_index(const char *path) {
    for (int i = 0; i < MOCK_FILES_MAX; i++) {
        if (g_files[i].path && strcmp(g_files[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

void mock_clear_boot_log(void) { g_boot_log_len = 0; g_boot_log[0] = '\0'; }

const char *mock_boot_log(void) { return g_boot_log; }
size_t mock_boot_log_len(void) { return g_boot_log_len; }

FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt) {
    (void)path; (void)opt;
    memcpy(fs, &g_fatfs, sizeof(FATFS));
    return FR_OK;
}

static FRESULT resolve(const char *path, const char **content, size_t *len, int *is_dir) {
    *is_dir = 0;
    for (int i = 0; i < MOCK_FILES_MAX; i++) {
        if (g_files[i].path && strcmp(g_files[i].path, path) == 0) {
            *content = g_files[i].data;
            *len = g_files[i].len;
            return FR_OK;
        }
    }
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
    if (g_ejected) return FR_NOT_READY;
    FRESULT r = resolve(path, &content, &len, &is_dir);
    if (r != FR_OK || is_dir) return r == FR_OK ? FR_DENIED : r;
    memset(fp, 0, sizeof(*fp));
    fp->obj.fs = &g_fatfs;
    int fi = g_file_index(path);
    if (fi >= 0) {
        fp->buf = (BYTE *)g_files[fi].data;
        fp->len = g_files[fi].len;
        fp->cap = g_files[fi].cap;
        fp->user = &g_files[fi];
        if (mode & FA_CREATE_ALWAYS) {
            fp->len = 0;
            g_files[fi].data[0] = '\0';
        }
        if (mode & FA_OPEN_APPEND) {
            fp->fptr = fp->len;
        }
    } else if (strcmp(path, "boot.log") == 0) {
        fp->buf = (BYTE *)g_boot_log;
        fp->cap = sizeof(g_boot_log);
        fp->user = NULL;
        if (mode & FA_CREATE_ALWAYS) g_boot_log_len = 0;
        fp->len = g_boot_log_len;
    } else {
        fp->buf = (BYTE *)content;
        fp->cap = 0; /* read-only */
        fp->user = NULL;
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
    size_t avail = fp->cap ? fp->cap - fp->len : 0;
    if (n > avail) n = avail;
    if (n > 0) {
        memcpy(fp->buf + fp->len, buff, n);
    }
    fp->len += n;
    if (fp->user) {
        /* Mock entry: keep the entry length in sync, NUL-terminate. */
        mock_file_t *entry = fp->user;
        entry->len = fp->len;
        entry->data[fp->len] = '\0';
    } else if (fp->buf == (BYTE *)g_boot_log) {
        g_boot_log_len = fp->len;
        g_boot_log[g_boot_log_len] = '\0';
    }
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
    if (g_ejected) return FR_NOT_READY;
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
    if (g_ejected) return FR_NOT_READY;
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
    if (g_ejected) return FR_NOT_READY;
    *nclst = 100;
    *fatfs = &g_fatfs;
    return FR_OK;
}

DWORD get_fattime(void) { return 0; }

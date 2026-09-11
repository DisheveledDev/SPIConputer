/* simulator/sim_fs.c
 *
 * FatFs-over-host-folder backend. Every f_* function maps to stdio /
 * POSIX directory calls under the configured root, so the OS's
 * fs_core0.c and the Lua fs module behave exactly as on hardware
 * (paths, errors, listing, whole-file reads) while files live on the
 * Mac's disk where they can be edited with normal tools.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
#include "sd_card.h"

#include "sim_dir.h"
#include "sim_fs.h"

static char s_root[1024];
static FATFS s_fatfs = {.csize = 64, .n_fatent = 65536};
static sd_card_t s_sd = {.pcName = "0:"};

const char *sim_fs_root(void) {
    return s_root;
}

sd_card_t *sd_get_by_num(size_t num) {
    return num == 0 ? &s_sd : NULL;
}

bool sd_init_driver(void) {
    return true;
}

/* ------------------------------------------------------------------ */
/* Root setup                                                          */
/* ------------------------------------------------------------------ */

static void mkdir_p(const char *path) {
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return false;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return false;
        }
    }
    fclose(in);
    fclose(out);
    return true;
}

bool sim_fs_init(const char *root, const char *seed_dir) {
    snprintf(s_root, sizeof(s_root), "%s", root);
    mkdir_p(s_root);
    struct stat st;
    if (stat(s_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    if (seed_dir) {
        static const char *seeds[] = {"os.lua", "editor.lua"};
        for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
            char dst[1200];
            snprintf(dst, sizeof(dst), "%s/%s", s_root, seeds[i]);
            if (stat(dst, &st) == 0) {
                continue; /* keep what the user has */
            }
            char src[1200];
            snprintf(src, sizeof(src), "%s/%s", seed_dir, seeds[i]);
            if (copy_file(src, dst)) {
                printf("[sim] seeded %s\n", seeds[i]);
            } else {
                printf("[sim] warning: cannot seed %s (looking in %s)\n",
                       seeds[i], seed_dir);
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Path translation                                                    */
/* ------------------------------------------------------------------ */

/* "0:/foo/bar.lua" -> "<root>/foo/bar.lua". Rejects ".." escapes. */
static bool sim_path(const char *path, char *out, size_t cap) {
    if (!path) {
        return false;
    }
    if (path[0] == '0' && path[1] == ':') {
        path += 2;
    }
    while (*path == '/' || *path == '\\') {
        path++;
    }
    if (strstr(path, "..")) {
        return false;
    }
    if (path[0] == '\0' || strlen(path) + strlen(s_root) + 2 > cap) {
        return false;
    }
    snprintf(out, cap, "%s/%s", s_root, path);
    return true;
}

static FRESULT errno_to_fr(void) {
    switch (errno) {
    case ENOENT:
        return FR_NO_FILE;
    case EACCES:
    case EPERM:
        return FR_DENIED;
    case EEXIST:
        return FR_EXIST;
    case ENOTDIR:
        return FR_NO_PATH;
    case ENOSPC:
        return FR_DENIED;
    default:
        return FR_INT_ERR;
    }
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE *fp;
    bool writable;
} sim_file_t;

FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt) {
    (void)path;
    (void)opt;
    if (fs) {
        *fs = s_fatfs;
    }
    s_sd.fatfs = s_fatfs;
    return FR_OK;
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode) {
    char full[1200];
    if (!sim_path(path, full, sizeof(full))) {
        return FR_INVALID_NAME;
    }
    bool rd = (mode & FA_READ) != 0;
    bool wr = (mode & FA_WRITE) != 0;
    const char *m;
    if (wr && (mode & FA_CREATE_ALWAYS)) {
        m = rd ? "w+b" : "wb";
    } else if (wr && (mode & FA_OPEN_APPEND)) {
        m = rd ? "a+b" : "ab";
    } else if (wr && (mode & FA_CREATE_NEW)) {
        m = rd ? "w+xb" : "wxb";
    } else if (wr && rd) {
        m = "r+b";
    } else if (wr) {
        m = "r+b";
    } else {
        m = "rb";
    }
    FILE *f = fopen(full, m);
    if (!f && wr && (mode & FA_OPEN_ALWAYS)) {
        f = fopen(full, rd ? "w+b" : "wb");
    }
    if (!f) {
        return errno_to_fr();
    }
    sim_file_t *sf = (sim_file_t *)calloc(1, sizeof(*sf));
    if (!sf) {
        fclose(f);
        return FR_NOT_ENOUGH_CORE;
    }
    sf->fp = f;
    sf->writable = wr;

    struct stat st;
    FSIZE_t size = 0;
    if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
        size = (FSIZE_t)st.st_size;
    }
    memset(fp, 0, sizeof(*fp));
    fp->user = sf;
    fp->len = size;
    fp->fptr = (mode & FA_OPEN_APPEND) ? size : 0;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
    sim_file_t *sf = (sim_file_t *)fp->user;
    if (!sf) {
        return FR_INVALID_OBJECT;
    }
    size_t n = fread(buff, 1, btr, sf->fp);
    fp->fptr += n;
    *br = (UINT)n;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
    sim_file_t *sf = (sim_file_t *)fp->user;
    if (!sf || !sf->writable) {
        return FR_DENIED;
    }
    size_t n = fwrite(buff, 1, btw, sf->fp);
    fp->fptr += n;
    if (fp->fptr > fp->len) {
        fp->len = fp->fptr;
    }
    *bw = (UINT)n;
    return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
    sim_file_t *sf = (sim_file_t *)fp->user;
    if (!sf) {
        return FR_INVALID_OBJECT;
    }
    if (!sf->writable && ofs > fp->len) {
        return FR_DENIED;
    }
    if (fseek(sf->fp, (long)ofs, SEEK_SET) != 0) {
        return FR_INT_ERR;
    }
    fp->fptr = ofs;
    return FR_OK;
}

FSIZE_t f_tell(FIL *fp) {
    return fp->fptr;
}

FSIZE_t f_size(FIL *fp) {
    return fp->len;
}

FRESULT f_sync(FIL *fp) {
    sim_file_t *sf = (sim_file_t *)fp->user;
    if (!sf) {
        return FR_INVALID_OBJECT;
    }
    fflush(sf->fp);
    return FR_OK;
}

FRESULT f_close(FIL *fp) {
    sim_file_t *sf = (sim_file_t *)fp->user;
    if (sf) {
        fclose(sf->fp);
        free(sf);
        fp->user = NULL;
    }
    return FR_OK;
}

/* ------------------------------------------------------------------ */
/* Directories                                                         */
/* ------------------------------------------------------------------ */

#define SIM_DIRS_MAX 4
static struct {
    void *d;
    char path[1200];
} s_dirs[SIM_DIRS_MAX];

FRESULT f_opendir(DIR *dp, const TCHAR *path) {
    char full[1200];
    if (!sim_path(path, full, sizeof(full))) {
        return FR_INVALID_NAME;
    }
    void *d = sim_dir_open(full);
    if (!d) {
        return errno_to_fr();
    }
    for (int i = 0; i < SIM_DIRS_MAX; i++) {
        if (!s_dirs[i].d) {
            s_dirs[i].d = d;
            snprintf(s_dirs[i].path, sizeof(s_dirs[i].path), "%s", full);
            dp->idx = i + 1;
            return FR_OK;
        }
    }
    sim_dir_close(d);
    return FR_TOO_MANY_OPEN_FILES;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno) {
    if (dp->idx < 1 || dp->idx > SIM_DIRS_MAX || !s_dirs[dp->idx - 1].d) {
        return FR_INVALID_OBJECT;
    }
    const char *dirpath = s_dirs[dp->idx - 1].path;
    char name[FF_LFN_BUF];
    if (!sim_dir_next(s_dirs[dp->idx - 1].d, name, sizeof(name))) {
        memset(fno, 0, sizeof(*fno));
        return FR_OK;
    }
    memset(fno, 0, sizeof(*fno));
    snprintf(fno->fname, sizeof(fno->fname), "%s", name);
    char buf[1300];
    snprintf(buf, sizeof(buf), "%s/%s", dirpath, name);
    struct stat st;
    if (stat(buf, &st) == 0) {
        fno->fsize = (FSIZE_t)st.st_size;
        if (S_ISDIR(st.st_mode)) {
            fno->fattrib |= AM_DIR;
        }
    }
    return FR_OK;
}

FRESULT f_closedir(DIR *dp) {
    if (dp->idx < 1 || dp->idx > SIM_DIRS_MAX || !s_dirs[dp->idx - 1].d) {
        return FR_INVALID_OBJECT;
    }
    sim_dir_close(s_dirs[dp->idx - 1].d);
    s_dirs[dp->idx - 1].d = NULL;
    s_dirs[dp->idx - 1].path[0] = '\0';
    dp->idx = 0;
    return FR_OK;
}

/* ------------------------------------------------------------------ */
/* Metadata                                                            */
/* ------------------------------------------------------------------ */

FRESULT f_stat(const TCHAR *path, FILINFO *fno) {
    char full[1200];
    if (!sim_path(path, full, sizeof(full))) {
        return FR_INVALID_NAME;
    }
    struct stat st;
    if (stat(full, &st) != 0) {
        return errno_to_fr();
    }
    memset(fno, 0, sizeof(*fno));
    const char *base = strrchr(full, '/');
    snprintf(fno->fname, sizeof(fno->fname), "%s", base ? base + 1 : full);
    fno->fsize = (FSIZE_t)st.st_size;
    if (S_ISDIR(st.st_mode)) {
        fno->fattrib |= AM_DIR;
    }
    return FR_OK;
}

FRESULT f_mkdir(const TCHAR *path) {
    char full[1200];
    if (!sim_path(path, full, sizeof(full))) {
        return FR_INVALID_NAME;
    }
    if (mkdir(full, 0755) != 0) {
        return errno_to_fr();
    }
    return FR_OK;
}

FRESULT f_unlink(const TCHAR *path) {
    char full[1200];
    if (!sim_path(path, full, sizeof(full))) {
        return FR_INVALID_NAME;
    }
    if (unlink(full) == 0) {
        return FR_OK;
    }
    if (errno == EISDIR || errno == EPERM) {
        if (rmdir(full) == 0) {
            return FR_OK;
        }
    }
    return errno_to_fr();
}

FRESULT f_rename(const TCHAR *old_path, const TCHAR *new_path) {
    char a[1200], b[1200];
    if (!sim_path(old_path, a, sizeof(a)) || !sim_path(new_path, b, sizeof(b))) {
        return FR_INVALID_NAME;
    }
    if (rename(a, b) != 0) {
        return errno_to_fr();
    }
    return FR_OK;
}

FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs) {
    (void)path;
    struct statvfs vfs;
    if (statvfs(s_root, &vfs) != 0) {
        return FR_INT_ERR;
    }
    /* csize counts 512-byte units in FatFs: 64 -> 32 KB clusters. */
    uint64_t total_kb = (uint64_t)vfs.f_blocks * vfs.f_frsize / 1024;
    uint64_t free_kb = (uint64_t)vfs.f_bavail * vfs.f_frsize / 1024;
    uint32_t total_clusters = (uint32_t)(total_kb / 32);
    *nclst = (DWORD)(free_kb / 32);
    s_fatfs.n_fatent = total_clusters + 2;
    *fatfs = &s_fatfs;
    return FR_OK;
}

DWORD get_fattime(void) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    return ((DWORD)(tmv.tm_year + 1900 - 1980) << 25) |
           ((DWORD)(tmv.tm_mon + 1) << 21) | ((DWORD)tmv.tm_mday << 16) |
           ((DWORD)tmv.tm_hour << 11) | ((DWORD)tmv.tm_min << 5) |
           ((DWORD)tmv.tm_sec / 2);
}

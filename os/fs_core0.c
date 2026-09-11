/* fs_core0.c
 *
 * Core 0 filesystem service (Phase 4): executes FatFs operations on
 * behalf of core 1 RPC requests. Platform-neutral apart from the FatFs
 * API itself, so the host tests run this exact code against the mock
 * FatFs layer.
 *
 * Handle pool: core 1 holds handle ids (1..FS_MAX_OPEN); the FIL
 * objects themselves stay in this file, owned by core 0.
 */
#include "fs_core0.h"

#include <string.h>

#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
#include "sd_card.h"

#include "rpc.h"

#define FS_MAX_OPEN 8

static FIL s_fil[FS_MAX_OPEN];
static bool s_open[FS_MAX_OPEN];
static bool s_mounted = false;

bool fs_core0_mount(void) {
    sd_card_t *sd = sd_get_by_num(0);
    if (!sd) {
        return false;
    }
    fs_core0_close_all();
    if (!sd_init_driver()) {
        s_mounted = false;
        return false;
    }
    FRESULT fr = f_mount(&sd->fatfs, sd->pcName, 1);
    s_mounted = (fr == FR_OK);
    return s_mounted;
}

bool fs_core0_mounted(void) {
    return s_mounted;
}

void fs_core0_close_all(void) {
    for (int i = 0; i < FS_MAX_OPEN; i++) {
        if (s_open[i]) {
            f_close(&s_fil[i]);
            s_open[i] = false;
        }
    }
}

static FIL *handle_get(int32_t h) {
    if (h < 1 || h > FS_MAX_OPEN || !s_open[h - 1]) {
        return NULL;
    }
    return &s_fil[h - 1];
}

static int32_t handle_alloc(void) {
    for (int i = 0; i < FS_MAX_OPEN; i++) {
        if (!s_open[i]) {
            s_open[i] = true;
            return (int32_t)(i + 1);
        }
    }
    return 0;
}

static void handle_free(int32_t h) {
    if (h >= 1 && h <= FS_MAX_OPEN) {
        s_open[h - 1] = false;
    }
}

/* Directory listing: NUL-terminated names, each followed by a
 * uint32 size and a uint8 dir flag, packed into the staging buffer.
 * Returns the entry count (0 if the buffer filled exactly). */
static uint16_t pack_ls(const char *path) {
    DIR dj;
    FILINFO fno;
    uint8_t *out = rpc_staging();
    size_t used = 0;
    uint16_t count = 0;

    if (f_opendir(&dj, path) != FR_OK) {
        return 0;
    }
    for (;;) {
        size_t nlen;
        FRESULT fr = f_readdir(&dj, &fno);
        if (fr != FR_OK || fno.fname[0] == 0) {
            break;
        }
        nlen = strlen(fno.fname) + 1;
        if (used + nlen + 5 > RPC_STAGING_SIZE) {
            break; /* buffer full; entries beyond are dropped */
        }
        memcpy(out + used, fno.fname, nlen);
        used += nlen;
        uint32_t sz = (uint32_t)fno.fsize;
        memcpy(out + used, &sz, 4);
        used += 4;
        out[used++] = (fno.fattrib & AM_DIR) ? 1 : 0;
        count++;
    }
    f_closedir(&dj);
    return count;
}

static void handle_request(const rpc_request_t *req, rpc_response_t *resp) {
    FIL *f;
    FRESULT fr = FR_OK;

    resp->result = FR_OK;
    resp->value = 0;
    resp->value2 = 0;

    switch (req->op) {
    case RPC_FS_READY:
        resp->value = s_mounted ? 1 : 0;
        break;

    case RPC_FS_MOUNT:
        resp->value = fs_core0_mount() ? 1 : 0;
        break;

    case RPC_FS_OPEN: {
        int32_t h = handle_alloc();
        if (h == 0) {
            resp->result = FR_TOO_MANY_OPEN_FILES;
            break;
        }
        fr = f_open(&s_fil[h - 1], req->path1, (BYTE)req->a);
        if (fr != FR_OK) {
            handle_free(h);
            resp->result = fr;
            break;
        }
        resp->value = h;
        break;
    }

    case RPC_FS_CLOSE:
        f = handle_get(req->a);
        if (!f) {
            resp->result = FR_INVALID_OBJECT;
            break;
        }
        fr = f_close(f);
        handle_free(req->a);
        resp->result = fr;
        break;

    case RPC_FS_READ: {
        UINT got = 0;
        f = handle_get(req->a);
        if (!f) {
            resp->result = FR_INVALID_OBJECT;
            break;
        }
        UINT want = (UINT)(req->b > RPC_STAGING_SIZE ? RPC_STAGING_SIZE
                                                     : req->b);
        fr = f_read(f, rpc_staging(), want, &got);
        resp->result = fr;
        resp->value = (int32_t)got;
        break;
    }

    case RPC_FS_WRITE: {
        UINT written = 0;
        f = handle_get(req->a);
        if (!f) {
            resp->result = FR_INVALID_OBJECT;
            break;
        }
        UINT want = (UINT)(req->b > RPC_STAGING_SIZE ? RPC_STAGING_SIZE
                                                     : req->b);
        fr = f_write(f, rpc_staging(), want, &written);
        resp->result = fr;
        resp->value = (int32_t)written;
        break;
    }

    case RPC_FS_SEEK: {
        f = handle_get(req->a);
        if (!f) {
            resp->result = FR_INVALID_OBJECT;
            break;
        }
        FSIZE_t base = 0;
        if (req->whence == 1) {
            base = f_tell(f);
        } else if (req->whence == 2) {
            base = f_size(f);
        } else if (req->whence != 0) {
            resp->result = FR_INVALID_PARAMETER;
            break;
        }
        fr = f_lseek(f, base + (FSIZE_t)req->b);
        resp->result = fr;
        if (fr == FR_OK) {
            resp->value = (int32_t)f_tell(f);
        }
        break;
    }

    case RPC_FS_TELL:
        f = handle_get(req->a);
        if (!f) {
            resp->result = FR_INVALID_OBJECT;
            break;
        }
        resp->value = (int32_t)f_tell(f);
        break;

    case RPC_FS_SIZE:
        f = handle_get(req->a);
        if (!f) {
            resp->result = FR_INVALID_OBJECT;
            break;
        }
        resp->value = (int32_t)f_size(f);
        break;

    case RPC_FS_FLUSH:
        f = handle_get(req->a);
        if (!f) {
            resp->result = FR_INVALID_OBJECT;
            break;
        }
        resp->result = f_sync(f);
        break;

    case RPC_FS_LS:
        resp->value = pack_ls(req->path1);
        break;

    case RPC_FS_STAT: {
        FILINFO fno;
        fr = f_stat(req->path1, &fno);
        resp->result = fr;
        if (fr == FR_OK) {
            resp->value = (int32_t)fno.fsize;
            resp->value2 = (fno.fattrib & AM_DIR) ? 1 : 0;
        }
        break;
    }

    case RPC_FS_EXISTS: {
        FILINFO fno;
        resp->value = (f_stat(req->path1, &fno) == FR_OK) ? 1 : 0;
        break;
    }

    case RPC_FS_MKDIR:
        resp->result = f_mkdir(req->path1);
        break;

    case RPC_FS_REMOVE:
        resp->result = f_unlink(req->path1);
        break;

    case RPC_FS_RENAME:
        resp->result = f_rename(req->path1, req->path2);
        break;

    case RPC_FS_FREE: {
        FATFS *fs;
        DWORD fre_clst = 0;
        fr = f_getfree("0:", &fre_clst, &fs);
        resp->result = fr;
        if (fr == FR_OK) {
            resp->value = (int32_t)((uint64_t)fre_clst * fs->csize / 2);
            resp->value2 =
                (int32_t)((uint64_t)(fs->n_fatent - 2) * fs->csize / 2);
        }
        break;
    }

    default:
        resp->result = FR_INVALID_PARAMETER;
        break;
    }
}

bool fs_core0_service(void) {
    if (!rpc_pending()) {
        return false;
    }
    rpc_response_t resp;
    handle_request(rpc_peek(), &resp);
    rpc_respond(&resp);
    return true;
}

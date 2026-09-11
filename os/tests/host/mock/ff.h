/* mock/ff.h — Mock FatFs API for host-side testing of the Lua bridge */
#ifndef MOCK_FF_H
#define MOCK_FF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef uint64_t QWORD;
typedef uint64_t FSIZE_t;
typedef const char TCHAR;

#define FF_LFN_BUF 260

typedef enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST,
    FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE,
    FR_NOT_ENABLED,
    FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED,
    FR_TIMEOUT,
    FR_LOCKED,
    FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES,
    FR_INVALID_PARAMETER
} FRESULT;

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_NEW 0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS 0x10
#define FA_OPEN_APPEND 0x30

#define AM_RDO 0x01
#define AM_HID 0x02
#define AM_SYS 0x04
#define AM_DIR 0x10
#define AM_ARC 0x20

typedef struct {
    void *fs;
    WORD id;
} FFOBJID;

typedef struct {
    FFOBJID obj;
    BYTE flag;
    BYTE err;
    FSIZE_t fptr;
    DWORD clust;
    DWORD sect;
    DWORD dir_sect;
    BYTE *dir_ptr;
    BYTE *buf;
    size_t len;
    size_t cap;  /* writable capacity (0 = read-only) */
    void *user;  /* owning mock entry, or NULL */
} FIL;

typedef struct {
    DWORD csize;
    DWORD n_fatent;
} FATFS;

typedef struct {
    int idx;
} DIR;

typedef struct {
    FSIZE_t fsize;
    WORD fdate;
    WORD ftime;
    BYTE fattrib;
    char altname[13];
    char fname[FF_LFN_BUF];
} FILINFO;

FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt);
FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FSIZE_t f_tell(FIL *fp);
FSIZE_t f_size(FIL *fp);
FRESULT f_sync(FIL *fp);
FRESULT f_close(FIL *fp);
FRESULT f_opendir(DIR *dp, const TCHAR *path);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);
FRESULT f_stat(const TCHAR *path, FILINFO *fno);
FRESULT f_mkdir(const TCHAR *path);
FRESULT f_unlink(const TCHAR *path);
FRESULT f_rename(const TCHAR *old_path, const TCHAR *new_path);
FRESULT f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs);
DWORD get_fattime(void);

#endif

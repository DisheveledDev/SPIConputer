/* mock/f_util.h — FRESULT_str helper for host tests */
#ifndef MOCK_F_UTIL_H
#define MOCK_F_UTIL_H

#include "ff.h"

static inline const char *FRESULT_str(FRESULT i) {
    switch (i) {
        case FR_OK: return "FR_OK";
        case FR_NO_FILE: return "FR_NO_FILE";
        case FR_NO_PATH: return "FR_NO_PATH";
        case FR_NOT_READY: return "FR_NOT_READY";
        case FR_DENIED: return "FR_DENIED";
        case FR_EXIST: return "FR_EXIST";
        default: return "FR_ERROR";
    }
}

#endif

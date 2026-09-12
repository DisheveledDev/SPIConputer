/* mock/hw_config.h — minimal hw_config for host tests */
#ifndef MOCK_HW_CONFIG_H
#define MOCK_HW_CONFIG_H

#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sd_card_t {
    const char *pcName;
    FATFS fatfs;
} sd_card_t;

sd_card_t *sd_get_by_num(size_t num);

#ifdef __cplusplus
}
#endif

#endif

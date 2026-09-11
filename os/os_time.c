/* os_time.c — see os_time.h */
#include "os_time.h"

#if defined(PICO_RP2350) || defined(PICO_RP2040)
#include "pico/time.h"

uint64_t os_time_us(void) {
    return time_us_64();
}

#else /* host build */

#include <time.h>

uint64_t os_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

#endif

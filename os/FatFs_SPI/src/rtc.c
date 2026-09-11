/* rtc.c
 *
 * Minimal RTC support for the RP2350 (Pico 2).
 *
 * The RP2350 has no battery-backed RTC with a calendar, so there is no
 * time source at boot. File timestamps written by FatFs are stamped with
 * a fixed, valid date (2025-01-01 00:00:00) until a real clock (NTP, RTC
 * module) is added.
 */
#include <time.h>
//
#include "ff.h"
//
#include "rtc.h"

static time_t epochtime;

time_t time(time_t *pxTime) {
    if (pxTime) {
        *pxTime = epochtime;
    }
    return epochtime;
}

void time_init() {
    epochtime = 0;
}

/* Called by FatFs. Returns a fixed valid timestamp: 2025-01-01 00:00:00. */
DWORD get_fattime(void) {
    DWORD fattime = 0;
    fattime |= (DWORD)(2025 - 1980) << 25; /* Year origin 1980 */
    fattime |= (DWORD)1 << 21;             /* Month (1..12) */
    fattime |= (DWORD)1 << 16;             /* Day (1..31) */
    /* Hour, minute and second/2 all zero. */
    return fattime;
}

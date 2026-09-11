/* os_time.h
 *
 * Monotonic microsecond clock used by the core 1 scheduler (timers,
 * pause/resume shifting) and exposed to Lua as TimeNow().
 *
 * Firmware: the RP2350/RP2040 system timer. Host tests: CLOCK_MONOTONIC.
 */
#pragma once

#include <stdint.h>

uint64_t os_time_us(void);

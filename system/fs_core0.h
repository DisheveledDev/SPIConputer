/* fs_core0.h
 *
 * Core 0 side of the SD/filesystem RPC (Phase 4). Core 0 is the only
 * owner of FatFs and the SPI bus: all f_* calls happen here, inside
 * fs_core0_service(), driven by RPC requests from core 1. FIL handles
 * never leave core 0; core 1 sees handle ids.
 */
#pragma once

#include <stdbool.h>

/* Mount the SD card (logical drive "0:"). Runs on core 0 only. */
bool fs_core0_mount(void);

/* True if the SD card is mounted (core 0 side). */
bool fs_core0_mounted(void);

/* Service one pending RPC request, if any. Returns true if a request
 * was serviced (used by core 0 to detect system progress for the
 * watchdog). */
bool fs_core0_service(void);

/* Close all open handles (e.g. before re-mounting). */
void fs_core0_close_all(void);

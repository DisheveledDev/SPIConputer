/* fs_core0.h
 *
 * FatFs side of the filesystem interface (Phase 4). This is the only
 * owner of FatFs and the SD SPI bus: all f_* calls happen here. FIL
 * handles never leave this file; the Lua side sees handle ids.
 *
 * fs_core0_execute() runs one request inline (the normal firmware path:
 * both sides are on the OS core). fs_core0_service() is the two-core
 * variant used by the host harness and the desktop simulator.
 */
#pragma once

#include <stdbool.h>

#include "rpc.h"

/* Execute one filesystem request. The caller has already copied any
 * outbound payload into rpc_staging(); inbound payload (if any) is in
 * rpc_staging() on return. */
void fs_core0_execute(const rpc_request_t *req, rpc_response_t *resp);

/* Mount the SD card (logical drive "0:"). */
bool fs_core0_mount(void);

bool fs_core0_write_error(const char *name, uint64_t timestamp,
                          const char *text);
bool fs_core0_debug_log(const char *line);

/* True if the SD card is mounted (core 0 side). */
bool fs_core0_mounted(void);

/* Service one pending RPC request, if any. Returns true if a request
 * was serviced (used by core 0 to detect system progress for the
 * watchdog). */
bool fs_core0_service(void);

/* Close all open handles (e.g. before re-mounting). */
void fs_core0_close_all(void);

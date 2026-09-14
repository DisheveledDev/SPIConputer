/* rpc.h
 *
 * Filesystem call interface (Phase 4). The op codes, request/response
 * shapes and the shared staging buffer are the contract between the Lua
 * bindings (`fs_lua.c`) and the FatFs layer (`fs_core0.c`).
 *
 * After the core split both sides run on the OS core, so the normal
 * firmware path is a direct call: `rpc_set_local_handler()` points at
 * `fs_core0_execute()` and `rpc_call()` dispatches inline. The original
 * two-core transport (a request slot, a response slot and the
 * wait/signal hooks) is kept for builds that still split the two, such
 * as the host harness and the desktop simulator; it is only used when no
 * local handler is installed.
 *
 * Data protocol: the caller copies outbound payload into the staging
 * buffer before rpc_call(); the callee fills the staging buffer with the
 * inbound payload before returning. Staging is RPC_STAGING_SIZE bytes,
 * owned by whichever side the protocol says.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RPC_STAGING_SIZE 4096
#define RPC_PATH_MAX 128

typedef enum {
    RPC_FS_READY = 0, /* -> value: 1 mounted / 0 not */
    RPC_FS_MOUNT,     /* (re)mount the SD card -> value: 1/0 */
    RPC_FS_OPEN,      /* path1 + a(FA_* flags) -> value: handle id (0 = fail) */
    RPC_FS_CLOSE,     /* a: handle */
    RPC_FS_READ,      /* a: handle, b: max bytes; staging <- data; value: got */
    RPC_FS_WRITE,     /* a: handle, b: count; staging -> data; value: written */
    RPC_FS_SEEK,      /* a: handle, b: offset, whence in u.whence; value: pos */
    RPC_FS_TELL,      /* a: handle -> value: pos */
    RPC_FS_SIZE,      /* a: handle -> value: size */
    RPC_FS_FLUSH,     /* a: handle */
    RPC_FS_LS,        /* path1 -> staging: entries; value: count */
    RPC_FS_FIND,      /* path1: dir, path2: name -> staging: real name;
                       * value: name length (0 = not found) */
    RPC_FS_STAT,      /* path1 -> value: size, value2: is_dir */
    RPC_FS_EXISTS,    /* path1 -> value: 1/0 */
    RPC_FS_MKDIR,     /* path1 */
    RPC_FS_REMOVE,    /* path1 */
    RPC_FS_RENAME,    /* path1 -> path2 */
    RPC_FS_FREE,      /* -> value: free_kb, value2: total_kb */
} rpc_op_t;

typedef struct {
    rpc_op_t op;
    int32_t a; /* handle, offset, flags, ... */
    int32_t b; /* count, ... */
    int32_t whence; /* 0 = set, 1 = cur, 2 = end */
    char path1[RPC_PATH_MAX];
    char path2[RPC_PATH_MAX];
} rpc_request_t;

typedef struct {
    int32_t result; /* FRESULT code; FR_OK (0) on success */
    int32_t value;  /* op-specific primary result */
    int32_t value2; /* op-specific secondary result */
} rpc_response_t;

/* Install the filesystem service to call directly. The firmware passes
 * `fs_core0_execute` because both sides run on the OS core; leaving it
 * unset keeps the two-core slot transport in use. */
void rpc_set_local_handler(
    void (*handler)(const rpc_request_t *req, rpc_response_t *resp));

/* Issue a filesystem call. The caller must have copied any outbound
 * payload into rpc_staging() before calling; after return, inbound
 * payload (if any) is in rpc_staging(). Returns 0 on success. */
int rpc_call(const rpc_request_t *req, rpc_response_t *resp);

/* Two-core transport (only used when no local handler is installed).
 * The service side polls and completes: rpc_respond copies the response
 * into the slot, marks it ready and wakes the caller. */
bool rpc_pending(void);
const rpc_request_t *rpc_peek(void);
void rpc_respond(const rpc_response_t *resp);

/* The shared staging buffer (RPC_STAGING_SIZE bytes). */
uint8_t *rpc_staging(void);

/* Bind the platform blocking/wakeup hooks (two-core builds only). */
void rpc_bind_wait(void (*wait)(void));
void rpc_bind_signal(void (*signal)(void));

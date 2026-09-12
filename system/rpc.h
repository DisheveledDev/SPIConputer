/* rpc.h
 *
 * Core 1 -> core 0 RPC transport (Phase 4). Core 1 (the Lua core) makes
 * synchronous blocking calls; core 0 services them in its main loop.
 * One request is in flight at a time, so a single slot plus a staging
 * buffer is all the state needed.
 *
 * Data protocol: the caller copies outbound payload into the staging
 * buffer before rpc_call(); the responder fills the staging buffer with
 * the inbound payload before rpc_respond(). Staging is
 * RPC_STAGING_SIZE bytes, owned by whichever side the protocol says.
 *
 * Platform hookup: g_system_state.rpc.wait/signal are bound at boot.
 * On firmware, wait blocks on a multicore semaphore (or spins on
 * __wfe) and signal wakes it; on the host test harness, wait runs the
 * core 0 service function inline and signal is a no-op.
 */
#pragma once

#include <stdatomic.h>
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

typedef struct {
    rpc_request_t request;
    atomic_uint request_ready; /* core 1 writes (release), core 0 reads */
    rpc_response_t response;
    atomic_uint response_ready; /* core 0 writes (release), core 1 reads */
    uint8_t staging[RPC_STAGING_SIZE];

    /* Platform hooks, bound at boot (see rpc_bind_wait/signal). */
    void (*wait)(void);
    void (*signal)(void);
} rpc_t;

/* Core 1 side: blocking call. The caller must have copied any outbound
 * payload into rpc_staging() before calling; after return, inbound
 * payload (if any) is in rpc_staging(). Returns 0 on success. */
int rpc_call(const rpc_request_t *req, rpc_response_t *resp);

/* Core 0 side: poll and complete. rpc_respond copies the response into
 * the slot, marks it ready and wakes core 1. */
bool rpc_pending(void);
const rpc_request_t *rpc_peek(void);
void rpc_respond(const rpc_response_t *resp);

/* The shared staging buffer (RPC_STAGING_SIZE bytes). */
uint8_t *rpc_staging(void);

/* Bind the platform blocking/wakeup hooks. */
void rpc_bind_wait(void (*wait)(void));
void rpc_bind_signal(void (*signal)(void));

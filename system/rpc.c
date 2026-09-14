/* rpc.c
 *
 * Filesystem call interface (see rpc.h). Two transports:
 *
 *   - direct: when a local handler is installed (the firmware after the
 *     core split), rpc_call() executes the filesystem operation inline,
 *     with no atomics, no wakeups and no other core involved;
 *   - slot: the original two-core transport, used by builds that still
 *     split the Lua bindings from FatFs (the host harness and the
 *     desktop simulator). The wait/signal hooks are bound at boot.
 *
 * The transport state lives here rather than in system_state, because it
 * is no longer shared state.
 */
#include "rpc.h"

#include <stdatomic.h>
#include <string.h>

/* The staging buffer, owned by the protocol and shared with
 * fs_core0.c's payload handling. */
static uint8_t s_staging[RPC_STAGING_SIZE];

/* Direct transport. */
static void (*s_local)(const rpc_request_t *req, rpc_response_t *resp);

/* Slot transport. */
typedef struct {
    rpc_request_t request;
    atomic_uint request_ready;
    rpc_response_t response;
    atomic_uint response_ready;
    void (*wait)(void);
    void (*signal)(void);
} rpc_slot_t;

static rpc_slot_t s_slot;

void rpc_set_local_handler(
    void (*handler)(const rpc_request_t *req, rpc_response_t *resp)) {
    s_local = handler;
}

void rpc_bind_wait(void (*wait)(void)) {
    s_slot.wait = wait;
}

void rpc_bind_signal(void (*signal)(void)) {
    s_slot.signal = signal;
}

uint8_t *rpc_staging(void) {
    return s_staging;
}

int rpc_call(const rpc_request_t *req, rpc_response_t *resp) {
    if (s_local) {
        /* Same core as the service: run it now. */
        s_local(req, resp);
        return 0;
    }

    /* One request in flight at a time: the slot must be empty. */
    memcpy(&s_slot.request, req, sizeof(*req));
    atomic_store_explicit(&s_slot.request_ready, 1, memory_order_release);

    if (s_slot.wait) {
        s_slot.wait();
    }

    memcpy(resp, &s_slot.response, sizeof(*resp));
    atomic_store_explicit(&s_slot.response_ready, 0, memory_order_relaxed);
    return 0;
}

bool rpc_pending(void) {
    return atomic_load_explicit(&s_slot.request_ready,
                                memory_order_acquire) != 0;
}

const rpc_request_t *rpc_peek(void) {
    return &s_slot.request;
}

void rpc_respond(const rpc_response_t *resp) {
    memcpy(&s_slot.response, resp, sizeof(*resp));
    atomic_store_explicit(&s_slot.response_ready, 1, memory_order_release);
    atomic_store_explicit(&s_slot.request_ready, 0, memory_order_relaxed);
    if (s_slot.signal) {
        s_slot.signal();
    }
}

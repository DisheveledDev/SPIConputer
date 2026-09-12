/* rpc.c
 *
 * RPC transport implementation. Platform-neutral: the blocking/wakeup
 * hooks are bound at boot by the firmware (core0/main.c) or the host
 * test harness (tests/host).
 */
#include "rpc.h"

#include <string.h>

#include "system_state.h"

void rpc_bind_wait(void (*wait)(void)) {
    g_system_state.rpc.wait = wait;
}

void rpc_bind_signal(void (*signal)(void)) {
    g_system_state.rpc.signal = signal;
}

uint8_t *rpc_staging(void) {
    return g_system_state.rpc.staging;
}

int rpc_call(const rpc_request_t *req, rpc_response_t *resp) {
    rpc_t *rpc = &g_system_state.rpc;

    /* One request in flight at a time: the slot must be empty. */
    memcpy(&rpc->request, req, sizeof(*req));
    atomic_store_explicit(&rpc->request_ready, 1, memory_order_release);

    if (rpc->wait) {
        rpc->wait();
    }

    memcpy(resp, &rpc->response, sizeof(*resp));
    atomic_store_explicit(&rpc->response_ready, 0, memory_order_relaxed);
    return 0;
}

bool rpc_pending(void) {
    return atomic_load_explicit(&g_system_state.rpc.request_ready,
                                memory_order_acquire) != 0;
}

const rpc_request_t *rpc_peek(void) {
    return &g_system_state.rpc.request;
}

void rpc_respond(const rpc_response_t *resp) {
    rpc_t *rpc = &g_system_state.rpc;

    memcpy(&rpc->response, resp, sizeof(*resp));
    atomic_store_explicit(&rpc->response_ready, 1, memory_order_release);
    atomic_store_explicit(&rpc->request_ready, 0, memory_order_relaxed);
    if (rpc->signal) {
        rpc->signal();
    }
}

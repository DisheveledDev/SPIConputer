/* system_state.h
 *
 * Everything shared across cores. Core 0 owns the producers (input
 * scanning, RS232, and later SD RPC and video rendering); core 1 owns
 * the consumer (the Lua scheduler). Each field documents its access
 * rule: single owner, or guarded by which primitive.
 *
 * Phase 3 scope: the input event queue. Video state, RPC queues and the
 * watchdog heartbeat land here in later phases.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "rpc.h"

/* Ring depth must stay a power of two. */
#define INPUT_QUEUE_DEPTH 128

/* Event types (input_event_t.type) */
#define INPUT_EV_KEY 1      /* key + mods + pressed */
#define INPUT_EV_CONTROL1 2 /* joystick 1: ctrl + dirs + pressed */
#define INPUT_EV_CONTROL2 3 /* joystick 2 */

/* Modifier bits (input_event_t.mods) */
#define INPUT_MOD_SHIFT 0x01
#define INPUT_MOD_CTRL 0x02
#define INPUT_MOD_CBM 0x04 /* Commodore key */
#define INPUT_MOD_RESTORE 0x08

/* Joystick direction bits (input_event_t.dirs) */
#define INPUT_DIR_UP 0x01
#define INPUT_DIR_DOWN 0x02
#define INPUT_DIR_LEFT 0x04
#define INPUT_DIR_RIGHT 0x08
#define INPUT_DIR_FIRE 0x10

/* Extended key codes for matrix keys without an ASCII equivalent.
 * (Provisional; see AGENTS.md open question 7.) */
#define INPUT_KEY_UP 128
#define INPUT_KEY_DOWN 129
#define INPUT_KEY_LEFT 130
#define INPUT_KEY_RIGHT 131
#define INPUT_KEY_F1 132
#define INPUT_KEY_F2 133
#define INPUT_KEY_F3 134
#define INPUT_KEY_F4 135
#define INPUT_KEY_F5 136
#define INPUT_KEY_F6 137
#define INPUT_KEY_F7 138
#define INPUT_KEY_HOME 139
#define INPUT_KEY_RUNSTOP 140

typedef struct {
    uint8_t type;    /* INPUT_EV_* */
    uint8_t key;     /* ASCII code, or INPUT_KEY_* for KEY events */
    uint8_t mods;    /* INPUT_MOD_* bits */
    uint8_t pressed; /* 1 = down, 0 = up */
    uint8_t ctrl;    /* controller number for CONTROL events (1/2) */
    uint8_t dirs;    /* INPUT_DIR_* bits */
} input_event_t;

/* Single-producer/single-consumer ring buffer.
 *
 * Producer: core 0 only (the 1 kHz input IRQ). Consumer: core 1 only
 * (the scheduler, between ticks). Head and tail are release/acquire
 * atomics so no lock is ever taken on the hot path. On overflow the
 * producer drops the oldest event.
 */
typedef struct {
    input_event_t events[INPUT_QUEUE_DEPTH];
    atomic_uint head; /* producer writes here (mod depth) */
    atomic_uint tail; /* consumer reads here (mod depth) */
} input_queue_t;

void input_queue_init(input_queue_t *q);

/* Producer side (core 0 only; IRQ context is fine). */
void input_queue_push(input_queue_t *q, const input_event_t *ev);

/* Consumer side (core 1 only). Returns false when empty. */
bool input_queue_pop(input_queue_t *q, input_event_t *ev);

/* The one shared state object. Grows as later phases land. */
typedef struct {
    input_queue_t input;
    rpc_t rpc;
    /* Core 1 liveness counter: incremented every scheduler loop by core
     * 1, read by core 0 which gates the hardware watchdog feed on it
     * (a stuck VM stops the feeds and resets the system). */
    atomic_uint heartbeat;
} system_state_t;

extern system_state_t g_system_state;

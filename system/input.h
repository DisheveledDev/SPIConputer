/* input.h
 *
 * Platform-neutral input engine (Phase 3): key matrix scanner with
 * debounce, joystick edge detection, and RESTORE handling.
 *
 * The engine owns the event ring; the platform glue (core1/input_hw.c)
 * provides GPIO access and the 1 kHz tick, and the host tests inject
 * simulated GPIO patterns through the same interface. Producer (the
 * 1 kHz IRQ) and consumer (the scheduler) are both on the OS core, so
 * the ring indices are plain words, not atomics.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define INPUT_DEBOUNCE_SAMPLES 3 /* consecutive 1 kHz samples */

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

/* Extended key codes for matrix keys without an ASCII equivalent. */
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

/* Event ring, single producer (the 1 kHz input IRQ) and single consumer
 * (the scheduler), both on the OS core. On overflow the producer drops
 * the oldest event. The indices are plain words: with both sides on one
 * core only the single-word update below needs to be atomic, which a
 * store is. */
typedef struct {
    input_event_t events[INPUT_QUEUE_DEPTH];
    volatile uint32_t head; /* producer writes here (mod depth) */
    volatile uint32_t tail; /* consumer reads here (mod depth) */
} input_queue_t;

void input_queue_init(input_queue_t *q);
void input_queue_push(input_queue_t *q, const input_event_t *ev);
bool input_queue_pop(input_queue_t *q, input_event_t *ev);

/* GPIO-ish matrix interface, injected by the glue or a host test.
 * scan() drives the 8 row lines from `rows_out` (one bit set at a time)
 * and returns the 8 column readings in bits 0..7. */
typedef struct {
    uint8_t (*scan)(uint8_t rows_out, void *ctx);
    void *ctx;
} input_matrix_io_t;

/* One matrix position. `base`/`shifted` are ASCII codes or INPUT_KEY_*
 * extended codes; flags mark modifier keys. */
typedef struct {
    uint8_t base;
    uint8_t shifted;
    uint8_t flags;
} input_key_def_t;

#define INPUT_KF_SHIFT 0x01
#define INPUT_KF_CTRL 0x02
#define INPUT_KF_CBM 0x04

void input_init(void);

/* 1 kHz tick: scan the matrix, debounce, emit KEY events. */
void input_matrix_tick(const input_matrix_io_t *io);

/* 1 kHz tick: debounce stick `stick` (0 or 1) and emit one CONTROL
 * event per direction/fire edge. `dirs` is the raw INPUT_DIR_* mask. */
void input_joystick_tick(uint8_t stick, uint8_t dirs);

/* 1 kHz tick: debounce the C64 RESTORE line (1 = pressed) and emit
 * modifier key events (key 0, INPUT_MOD_RESTORE set while held). */
void input_restore_tick(uint8_t pressed);

/* The key definition table (row-major 8x8), exposed for host tests. */
extern const input_key_def_t input_key_table[64];

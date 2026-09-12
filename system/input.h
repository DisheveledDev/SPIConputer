/* input.h
 *
 * Platform-neutral input engine (Phase 3): key matrix scanner with
 * debounce, joystick edge detection, and RESTORE handling.
 * All pushes go to the core-0 side of the shared input queue; the
 * platform glue (core0/input_hw.c) provides GPIO access, and the host
 * tests inject simulated GPIO patterns through the same interface.
 */
#pragma once

#include <stdint.h>

#include "system_state.h"

#define INPUT_DEBOUNCE_SAMPLES 3 /* consecutive 1 kHz samples */

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

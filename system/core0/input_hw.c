/* core0/input_hw.c
 *
 * Core 0 hardware glue for the input subsystem (Phase 3).
 *
 * A 1 kHz repeating timer on core 0 is the single input tick: it scans
 * the C64 key matrix and polls both joysticks. All queue pushes happen
 * here (IRQ context), keeping the SPSC single-producer guarantee.
 */
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "board_config.h"
#include "input.h"

#if defined(SPICOMPUTER_HAS_KEYMATRIX)

/* Matrix wiring (active-low, like the real C64): one row driven low at
 * a time (GP28-35), columns pulled up and read (GP20-27); a pressed
 * key pulls its column low. Column order matches the C64 PB0-7 lines
 * and row order the PA0-7 lines, so input_key_table maps directly. */
static uint8_t scan_matrix(uint8_t rows_out, void *ctx) {
    int i;
    uint8_t cols = 0;
    (void)ctx;

    for (i = 0; i < KB_ROWS; i++) {
        gpio_put(KB_ROW_PIN(i), (rows_out >> i) & 1 ? 0 : 1);
    }
    for (i = 0; i < KB_COLS; i++) {
        if (!gpio_get(KB_COL_PIN(i))) {
            cols |= (uint8_t)(1u << i);
        }
    }
    return cols;
}

#endif /* SPICOMPUTER_HAS_KEYMATRIX */

#if defined(SPICOMPUTER_HAS_RESTORE)

/* RESTORE is its own line (active low, pulled up), like the real C64. */
static uint8_t read_restore(void) {
    return gpio_get(RESTORE_PIN) ? 0 : 1;
}

#endif /* SPICOMPUTER_HAS_RESTORE */

#if defined(SPICOMPUTER_HAS_JOYSTICKS)

/* Active-low Atari-style sticks: switch to GND, pins pulled up. */
static const uint8_t JOY1_PINS[5] = {
    JOY1_UP_PIN, JOY1_DOWN_PIN, JOY1_LEFT_PIN, JOY1_RIGHT_PIN, JOY1_FIRE_PIN};
static const uint8_t JOY2_PINS[5] = {
    JOY2_UP_PIN, JOY2_DOWN_PIN, JOY2_LEFT_PIN, JOY2_RIGHT_PIN, JOY2_FIRE_PIN};

static uint8_t read_joystick(const uint8_t pins[5]) {
    uint8_t dirs = 0;
    if (!gpio_get(pins[0])) dirs |= INPUT_DIR_UP;
    if (!gpio_get(pins[1])) dirs |= INPUT_DIR_DOWN;
    if (!gpio_get(pins[2])) dirs |= INPUT_DIR_LEFT;
    if (!gpio_get(pins[3])) dirs |= INPUT_DIR_RIGHT;
    if (!gpio_get(pins[4])) dirs |= INPUT_DIR_FIRE;
    return dirs;
}

#endif /* SPICOMPUTER_HAS_JOYSTICKS */

static bool input_tick_1khz(repeating_timer_t *rt) {
    (void)rt;

#if defined(SPICOMPUTER_HAS_KEYMATRIX)
    static const input_matrix_io_t matrix_io = {scan_matrix, NULL};
    input_matrix_tick(&matrix_io);
#endif

#if defined(SPICOMPUTER_HAS_RESTORE)
    input_restore_tick(read_restore());
#endif

#if defined(SPICOMPUTER_HAS_JOYSTICKS)
    input_joystick_tick(0, read_joystick(JOY1_PINS));
    input_joystick_tick(1, read_joystick(JOY2_PINS));
#endif

    return true;
}

void input_hw_init(void) {
    input_init();

#if defined(SPICOMPUTER_HAS_KEYMATRIX)
    for (int i = 0; i < KB_ROWS; i++) {
        gpio_init(KB_ROW_PIN(i));
        gpio_set_dir(KB_ROW_PIN(i), GPIO_OUT);
        gpio_put(KB_ROW_PIN(i), 1); /* idle: rows high */
    }
    for (int i = 0; i < KB_COLS; i++) {
        gpio_init(KB_COL_PIN(i));
        gpio_set_dir(KB_COL_PIN(i), GPIO_IN);
        gpio_pull_up(KB_COL_PIN(i));
    }
#endif

#if defined(SPICOMPUTER_HAS_RESTORE)
    gpio_init(RESTORE_PIN);
    gpio_set_dir(RESTORE_PIN, GPIO_IN);
    gpio_pull_up(RESTORE_PIN);
#endif

#if defined(SPICOMPUTER_HAS_JOYSTICKS)
    for (int i = 0; i < 5; i++) {
        gpio_init(JOY1_PINS[i]);
        gpio_set_dir(JOY1_PINS[i], GPIO_IN);
        gpio_pull_up(JOY1_PINS[i]);
        gpio_init(JOY2_PINS[i]);
        gpio_set_dir(JOY2_PINS[i], GPIO_IN);
        gpio_pull_up(JOY2_PINS[i]);
    }
#endif

    static repeating_timer_t timer;
    add_repeating_timer_us(1000, input_tick_1khz, NULL, &timer);
}

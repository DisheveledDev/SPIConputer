/* input_test.c
 *
 * Host-side tests for the input engine (input.c): the SPSC event queue
 * (including producer-flood behaviour), the matrix scanner debounce and
 * shift handling, joystick edge detection, and RS232 `input=` line
 * handling. GPIO access is simulated through input_matrix_io_t, so no
 * hardware is needed.
 *
 * Build and run:
 *   cmake -S tests/host -B build-host
 *   cmake --build build-host
 *   ./build-host/spicomputer_input_tests
 */
#include <stdio.h>
#include <string.h>

#include "input.h"
#include "system_state.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---------------- test 1: queue round trip and flood ---------------- */

static void test_queue(void) {
    input_queue_t q;
    input_event_t ev;
    int i, n;

    input_queue_init(&q);

    /* Round trip */
    for (i = 0; i < 4; i++) {
        input_event_t out;
        memset(&out, 0, sizeof(out));
        ev.type = INPUT_EV_KEY;
        ev.key = (uint8_t)('a' + i);
        ev.mods = 0;
        ev.pressed = 1;
        ev.ctrl = 0;
        ev.dirs = 0;
        input_queue_push(&q, &ev);
        CHECK(input_queue_pop(&q, &out), "pop after push");
        CHECK(out.key == (uint8_t)('a' + i), "round-trip key value");
    }
    CHECK(!input_queue_pop(&q, &ev), "queue empty after drain");

    /* Producer flood: push 1000 events without consuming. The queue
     * must drop the oldest and keep the last DEPTH-1 intact. */
    for (i = 0; i < 1000; i++) {
        ev.type = INPUT_EV_KEY;
        ev.key = (uint8_t)(i & 0xff);
        ev.mods = 0;
        ev.pressed = 1;
        ev.ctrl = 0;
        ev.dirs = 0;
        input_queue_push(&q, &ev);
    }
    n = 0;
    while (input_queue_pop(&q, &ev)) {
        /* Survivors are exactly the last DEPTH-1 pushed keys. */
        CHECK(ev.key == (uint8_t)((1000 - INPUT_QUEUE_DEPTH + 1 + n) & 0xff),
              "flood survivor ordering");
        n++;
    }
    CHECK(n == INPUT_QUEUE_DEPTH - 1, "flood leaves DEPTH-1 events");
    CHECK(!input_queue_pop(&q, &ev), "queue empty after flood drain");
}

/* ---------------- test 2: matrix debounce ---------------- */

typedef struct {
    uint8_t cols[8]; /* per-row column pattern (bit set = pressed) */
} fake_matrix_t;

static uint8_t fake_scan(uint8_t rows_out, void *ctx) {
    fake_matrix_t *fm = ctx;
    int r;
    for (r = 0; r < 8; r++) {
        if (rows_out & (1u << r)) {
            return fm->cols[r];
        }
    }
    return 0;
}

/* Debounced press: 2 ticks produce nothing, the 3rd produces down; the
 * same for release. */
static void test_matrix_debounce(void) {
    fake_matrix_t fm;
    input_matrix_io_t io = {fake_scan, &fm};
    input_event_t ev;
    int i;

    memset(&fm, 0, sizeof(fm));
    input_init();

    /* Key (1,1) = 'w' pressed: glitch on tick 1, solid from tick 2. */
    fm.cols[1] = 1u << 1;
    input_matrix_tick(&io); /* glitch sample 1 */
    CHECK(!input_queue_pop(&g_system_state.input, &ev),
          "no event after 1 sample");
    input_matrix_tick(&io); /* sample 2 */
    CHECK(!input_queue_pop(&g_system_state.input, &ev),
          "no event after 2 samples");
    input_matrix_tick(&io); /* sample 3 -> down */
    CHECK(input_queue_pop(&g_system_state.input, &ev), "down event on 3rd");
    CHECK(ev.type == INPUT_EV_KEY && ev.key == 'w' && ev.pressed == 1,
          "down event is 'w'");
    CHECK(ev.mods == 0, "no mods on plain key");

    /* Holding produces no repeats. */
    for (i = 0; i < 10; i++) {
        input_matrix_tick(&io);
    }
    CHECK(!input_queue_pop(&g_system_state.input, &ev),
          "no repeat while held");

    /* Release: 3 quiet samples -> up. */
    fm.cols[1] = 0;
    input_matrix_tick(&io);
    input_matrix_tick(&io);
    CHECK(!input_queue_pop(&g_system_state.input, &ev),
          "no up event after 2 quiet samples");
    input_matrix_tick(&io);
    CHECK(input_queue_pop(&g_system_state.input, &ev), "up event on 3rd");
    CHECK(ev.type == INPUT_EV_KEY && ev.key == 'w' && ev.pressed == 0,
          "up event is 'w'");
}

/* ---------------- test 3: shift produces capitals ---------------- */

static void test_matrix_shift(void) {
    fake_matrix_t fm;
    input_matrix_io_t io = {fake_scan, &fm};
    input_event_t ev;

    memset(&fm, 0, sizeof(fm));
    input_init();

    /* LEFT SHIFT at (1,7) held for 3 ticks. */
    fm.cols[1] = 1u << 7;
    input_matrix_tick(&io);
    input_matrix_tick(&io);
    input_matrix_tick(&io);
    CHECK(input_queue_pop(&g_system_state.input, &ev), "shift down event");
    CHECK(ev.type == INPUT_EV_KEY && ev.key == 0 &&
              (ev.mods & INPUT_MOD_SHIFT) && ev.pressed == 1,
          "shift key reports MOD_SHIFT with key 0");

    /* 'w' pressed while shift held -> 'W' with MOD_SHIFT. */
    fm.cols[1] |= 1u << 1;
    input_matrix_tick(&io);
    input_matrix_tick(&io);
    input_matrix_tick(&io);
    CHECK(input_queue_pop(&g_system_state.input, &ev), "'w' down event");
    CHECK(ev.key == 'W' && (ev.mods & INPUT_MOD_SHIFT) && ev.pressed == 1,
          "shifted 'w' is 'W' with MOD_SHIFT");

    /* Release shift: mods clear. */
    fm.cols[1] = 1u << 1; /* 'w' still held */
    input_matrix_tick(&io);
    input_matrix_tick(&io);
    input_matrix_tick(&io);
    CHECK(input_queue_pop(&g_system_state.input, &ev), "shift up event");
    CHECK(ev.key == 0 && !(ev.mods & INPUT_MOD_SHIFT) && ev.pressed == 0,
          "shift release clears MOD_SHIFT");

    /* Clean up 'w'. */
    fm.cols[1] = 0;
    input_matrix_tick(&io);
    input_matrix_tick(&io);
    input_matrix_tick(&io);
    CHECK(input_queue_pop(&g_system_state.input, &ev), "'w' up event");
    CHECK(ev.key == 'w' && ev.pressed == 0, "up event unshifted");
}

/* ---------------- test 4: joystick edges ---------------- */

static void test_joystick(void) {
    input_event_t ev;

    input_init();

    /* Stick 1: UP held 3 ticks -> one UP-down edge. */
    input_joystick_tick(0, INPUT_DIR_UP);
    input_joystick_tick(0, INPUT_DIR_UP);
    CHECK(!input_queue_pop(&g_system_state.input, &ev),
          "no joystick event before debounce");
    input_joystick_tick(0, INPUT_DIR_UP);
    CHECK(input_queue_pop(&g_system_state.input, &ev), "UP edge event");
    CHECK(ev.type == INPUT_EV_CONTROL1 && ev.ctrl == 1 &&
              ev.dirs == INPUT_DIR_UP && ev.pressed == 1,
          "UP edge is CONTROL1 dirs=UP pressed=1");

    /* Add FIRE while UP held: only the FIRE edge is reported. */
    input_joystick_tick(0, INPUT_DIR_UP | INPUT_DIR_FIRE);
    input_joystick_tick(0, INPUT_DIR_UP | INPUT_DIR_FIRE);
    CHECK(!input_queue_pop(&g_system_state.input, &ev),
          "no event before fire debounce");
    input_joystick_tick(0, INPUT_DIR_UP | INPUT_DIR_FIRE);
    CHECK(input_queue_pop(&g_system_state.input, &ev), "FIRE edge event");
    CHECK(ev.dirs == INPUT_DIR_FIRE && ev.pressed == 1,
          "only FIRE edge reported");

    /* Release everything: UP and FIRE release edges. */
    input_joystick_tick(0, 0);
    input_joystick_tick(0, 0);
    input_joystick_tick(0, 0);
    CHECK(input_queue_pop(&g_system_state.input, &ev), "release edge 1");
    CHECK(input_queue_pop(&g_system_state.input, &ev), "release edge 2");
}

/* ---------------- test 5: RESTORE line ---------------- */

static void test_restore(void) {
    input_event_t ev;

    input_init();

    input_restore_tick(1);
    input_restore_tick(1);
    CHECK(!input_queue_pop(&g_system_state.input, &ev),
          "no restore event before debounce");
    input_restore_tick(1);
    CHECK(input_queue_pop(&g_system_state.input, &ev), "restore down");
    CHECK(ev.type == INPUT_EV_KEY && ev.key == 0 &&
              (ev.mods & INPUT_MOD_RESTORE) && ev.pressed == 1,
          "restore reports MOD_RESTORE with key 0");

    input_restore_tick(0);
    input_restore_tick(0);
    CHECK(!input_queue_pop(&g_system_state.input, &ev),
          "no release before debounce");
    input_restore_tick(0);
    CHECK(input_queue_pop(&g_system_state.input, &ev), "restore up");
    CHECK(ev.key == 0 && !(ev.mods & INPUT_MOD_RESTORE) && ev.pressed == 0,
          "restore release clears the bit");
}

/* ---------------- test 6: RS232 input= lines ---------------- */

static void test_rs232(void) {
    input_event_t ev;
    const char *bare = "input=98\n"; /* 'b': synthesised down+up */
    const char *explicit_down = "input=98,1\n";
    const char *explicit_up = "input=98,0\n";
    const char *bad = "input=300\n";
    const char *garbage = "resolution=40x30\n";
    size_t i;

    input_init();

    for (i = 0; i < strlen(bare); i++) {
        input_rs232_feed((uint8_t)bare[i]);
    }
    CHECK(input_queue_pop(&g_system_state.input, &ev), "bare code down");
    CHECK(ev.type == INPUT_EV_KEY && ev.key == 98 && ev.pressed == 1,
          "bare down event");
    CHECK(input_queue_pop(&g_system_state.input, &ev), "bare code up");
    CHECK(ev.key == 98 && ev.pressed == 0, "bare up event");

    for (i = 0; i < strlen(explicit_down); i++) {
        input_rs232_feed((uint8_t)explicit_down[i]);
    }
    for (i = 0; i < strlen(explicit_up); i++) {
        input_rs232_feed((uint8_t)explicit_up[i]);
    }
    CHECK(input_queue_pop(&g_system_state.input, &ev), "explicit down");
    CHECK(ev.key == 98 && ev.pressed == 1, "explicit down event");
    CHECK(input_queue_pop(&g_system_state.input, &ev), "explicit up");
    CHECK(ev.key == 98 && ev.pressed == 0, "explicit up event");

    /* Invalid and non-input records produce nothing. */
    for (i = 0; i < strlen(bad); i++) {
        input_rs232_feed((uint8_t)bad[i]);
    }
    for (i = 0; i < strlen(garbage); i++) {
        input_rs232_feed((uint8_t)garbage[i]);
    }
    CHECK(!input_queue_pop(&g_system_state.input, &ev),
          "malformed input lines ignored");
}

int main(void) {
    printf("=== input engine tests ===\n");
    test_queue();
    test_matrix_debounce();
    test_matrix_shift();
    test_joystick();
    test_restore();
    test_rs232();
    if (g_failures == 0) {
        printf("all input tests passed\n");
        return 0;
    }
    printf("%d input test(s) FAILED\n", g_failures);
    return 1;
}

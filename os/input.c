/* input.c
 *
 * Platform-neutral input engine (Phase 3). No Pico headers here: the
 * matrix GPIO access is injected through input_matrix_io_t by the
 * platform glue, so the debounce/decoding logic is host-testable
 * (tests/host/input_test.c).
 *
 * All events are pushed to g_system_state.input by the core 0 producer
 * (the 1 kHz input IRQ); core 1 drains the queue between ticks in a
 * later phase.
 */
#include "input.h"

#include <string.h>

#include "protocol.h"

/* ---------------------------------------------------------------- */
/* Event queue (SPSC ring, drop-oldest on overflow)                 */
/* ---------------------------------------------------------------- */

void input_queue_init(input_queue_t *q) {
    memset(q->events, 0, sizeof(q->events));
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
}

void input_queue_push(input_queue_t *q, const input_event_t *ev) {
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    uint32_t next = (head + 1) & (INPUT_QUEUE_DEPTH - 1);

    if (next == tail) {
        /* Full: drop the oldest event. */
        tail = (tail + 1) & (INPUT_QUEUE_DEPTH - 1);
        atomic_store_explicit(&q->tail, tail, memory_order_relaxed);
    }
    q->events[head] = *ev;
    atomic_store_explicit(&q->head, next, memory_order_release);
}

bool input_queue_pop(input_queue_t *q, input_event_t *ev) {
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (tail == head) {
        return false;
    }
    *ev = q->events[tail];
    atomic_store_explicit(&q->tail,
                          (tail + 1) & (INPUT_QUEUE_DEPTH - 1),
                          memory_order_release);
    return true;
}

/* ---------------------------------------------------------------- */
/* Key table: C64 8x8 matrix, row-major (row = index/8, col = index%8).
 * Base/shifted values are provisional PC-style mappings; see AGENTS.md
 * open question 7. Modifier keys carry code 0 and their flag bit. */
/* ---------------------------------------------------------------- */

#define K(base, shifted, flags) {base, shifted, flags}

const input_key_def_t input_key_table[64] = {
    /* row 0: DEL, RETURN, CRSR RIGHT, F7, F1, F3, F5, CRSR DOWN */
    K(8, 8, 0),
    K(13, 13, 0),
    K(INPUT_KEY_RIGHT, INPUT_KEY_RIGHT, 0),
    K(INPUT_KEY_F7, INPUT_KEY_F7, 0),
    K(INPUT_KEY_F1, INPUT_KEY_F1, 0),
    K(INPUT_KEY_F3, INPUT_KEY_F3, 0),
    K(INPUT_KEY_F5, INPUT_KEY_F5, 0),
    K(INPUT_KEY_DOWN, INPUT_KEY_DOWN, 0),
    /* row 1: 3, W, A, 4, Z, S, E, LEFT SHIFT */
    K('3', '#', 0),
    K('w', 'W', 0),
    K('a', 'A', 0),
    K('4', '$', 0),
    K('z', 'Z', 0),
    K('s', 'S', 0),
    K('e', 'E', 0),
    K(0, 0, INPUT_KF_SHIFT),
    /* row 2: 5, R, D, 6, C, F, T, X */
    K('5', '%', 0),
    K('r', 'R', 0),
    K('d', 'D', 0),
    K('6', '^', 0),
    K('c', 'C', 0),
    K('f', 'F', 0),
    K('t', 'T', 0),
    K('x', 'X', 0),
    /* row 3: 7, Y, G, 8, B, H, U, V */
    K('7', '&', 0),
    K('y', 'Y', 0),
    K('g', 'G', 0),
    K('8', '*', 0),
    K('b', 'B', 0),
    K('h', 'H', 0),
    K('u', 'U', 0),
    K('v', 'V', 0),
    /* row 4: 9, I, J, 0, M, K, O, N */
    K('9', '(', 0),
    K('i', 'I', 0),
    K('j', 'J', 0),
    K('0', ')', 0),
    K('m', 'M', 0),
    K('k', 'K', 0),
    K('o', 'O', 0),
    K('n', 'N', 0),
    /* row 5: +, P, L, -, ., :, @, , */
    K('+', '+', 0),
    K('p', 'P', 0),
    K('l', 'L', 0),
    K('-', '_', 0),
    K('.', '>', 0),
    K(':', '[', 0),
    K('@', '`', 0),
    K(',', '<', 0),
    /* row 6: GBP, *, ;, HOME, RIGHT SHIFT, =, UP ARROW, / */
    K('#', '~', 0),
    K('*', '*', 0),
    K(';', ']', 0),
    K(INPUT_KEY_HOME, INPUT_KEY_HOME, 0),
    K(0, 0, INPUT_KF_SHIFT),
    K('=', '+', 0),
    K(INPUT_KEY_UP, INPUT_KEY_UP, 0),
    K('/', '?', 0),
    /* row 7: 1, LEFT ARROW, CTRL, 2, SPACE, C=, Q, RUN/STOP */
    K('1', '!', 0),
    K(INPUT_KEY_LEFT, INPUT_KEY_LEFT, 0),
    K(0, 0, INPUT_KF_CTRL),
    K('2', '@', 0),
    K(' ', ' ', 0),
    K(0, 0, INPUT_KF_CBM),
    K('q', 'Q', 0),
    K(INPUT_KEY_RUNSTOP, INPUT_KEY_RUNSTOP, 0),
};

/* ---------------------------------------------------------------- */
/* Key matrix scanner with N-sample debounce                         */
/* ---------------------------------------------------------------- */

static uint8_t s_raw[64];   /* last raw sample per key */
static uint8_t s_held[64];  /* debounced logical state */
static uint8_t s_count[64]; /* consecutive samples agreeing with s_raw */
static uint8_t s_mods;      /* currently held modifier bits */

static void emit_key(uint8_t key, uint8_t mods, uint8_t pressed) {
    input_event_t ev;
    ev.type = INPUT_EV_KEY;
    ev.key = key;
    ev.mods = mods;
    ev.pressed = pressed;
    ev.ctrl = 0;
    ev.dirs = 0;
    input_queue_push(&g_system_state.input, &ev);
}

static void handle_matrix_key(int k, uint8_t pressed) {
    const input_key_def_t *def = &input_key_table[k];
    uint8_t mods = s_mods;
    uint8_t code;

    if (def->flags & INPUT_KF_SHIFT) {
        if (pressed) {
            s_mods |= INPUT_MOD_SHIFT;
        } else {
            s_mods &= (uint8_t)~INPUT_MOD_SHIFT;
        }
        mods = s_mods;
        code = 0; /* modifier keys report themselves via the mods bits */
    } else if (def->flags & INPUT_KF_CTRL) {
        if (pressed) {
            s_mods |= INPUT_MOD_CTRL;
        } else {
            s_mods &= (uint8_t)~INPUT_MOD_CTRL;
        }
        mods = s_mods;
        code = 0;
    } else if (def->flags & INPUT_KF_CBM) {
        if (pressed) {
            s_mods |= INPUT_MOD_CBM;
        } else {
            s_mods &= (uint8_t)~INPUT_MOD_CBM;
        }
        mods = s_mods;
        code = 0;
    } else {
        code = (s_mods & INPUT_MOD_SHIFT) ? def->shifted : def->base;
        if (code == 0) {
            return;
        }
    }
    emit_key(code, mods, pressed);
}

void input_matrix_tick(const input_matrix_io_t *io) {
    int r, c;

    for (r = 0; r < 8; r++) {
        uint8_t cols = io->scan((uint8_t)(1u << r), io->ctx);
        for (c = 0; c < 8; c++) {
            int k = r * 8 + c;
            uint8_t bit = (cols >> c) & 1;

            if (bit == s_raw[k]) {
                if (s_count[k] < 255) {
                    s_count[k]++;
                }
            } else {
                s_raw[k] = bit;
                s_count[k] = 1;
            }
            if (bit && !s_held[k] && s_count[k] >= INPUT_DEBOUNCE_SAMPLES) {
                s_held[k] = 1;
                handle_matrix_key(k, 1);
            } else if (!bit && s_held[k] &&
                       s_count[k] >= INPUT_DEBOUNCE_SAMPLES) {
                s_held[k] = 0;
                handle_matrix_key(k, 0);
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* Joysticks: N-sample debounce, one event per direction edge        */
/* ---------------------------------------------------------------- */

typedef struct {
    uint8_t raw;
    uint8_t stable;
    uint8_t count;
} joy_state_t;

static joy_state_t s_joy[2];

void input_joystick_tick(uint8_t stick, uint8_t dirs) {
    joy_state_t *st;
    uint8_t changed;
    uint8_t bit;

    if (stick > 1) {
        return;
    }
    st = &s_joy[stick];
    if (dirs == st->raw) {
        if (st->count < 255) {
            st->count++;
        }
    } else {
        st->raw = dirs;
        st->count = 1;
    }
    if (st->count < INPUT_DEBOUNCE_SAMPLES) {
        return;
    }
    changed = st->stable ^ st->raw;
    if (changed == 0) {
        return;
    }
    for (bit = 0x01; bit != 0; bit <<= 1) {
        if (changed & bit) {
            input_event_t ev;
            ev.type = (stick == 0) ? INPUT_EV_CONTROL1 : INPUT_EV_CONTROL2;
            ev.key = 0;
            ev.mods = 0;
            ev.pressed = (st->raw & bit) ? 1 : 0;
            ev.ctrl = stick + 1;
            ev.dirs = bit;
            input_queue_push(&g_system_state.input, &ev);
        }
    }
    st->stable = st->raw;
}

/* ---------------------------------------------------------------- */
/* RS232 keyboard-in: `input=` lines (same parser as the terminal)   */
/* ---------------------------------------------------------------- */

static proto_parser_t s_rs232_parser;

void input_rs232_feed(uint8_t byte) {
    proto_record_t rec;
    size_t used = proto_parse(&s_rs232_parser, &byte, 1, &rec);

    if (used == 0 || rec.type != PROTO_REC_INPUT) {
        return;
    }
    if (rec.u.input.pressed == 2) {
        /* Bare code: synthesise down+up. */
        emit_key(rec.u.input.key, 0, 1);
        emit_key(rec.u.input.key, 0, 0);
    } else {
        emit_key(rec.u.input.key, 0, rec.u.input.pressed);
    }
}

/* ---------------------------------------------------------------- */
/* RESTORE line: separate from the matrix, like the real C64.        */
/* ---------------------------------------------------------------- */

static uint8_t s_restore_raw;
static uint8_t s_restore_held;
static uint8_t s_restore_count;

void input_restore_tick(uint8_t pressed) {
    if (pressed == s_restore_raw) {
        if (s_restore_count < 255) {
            s_restore_count++;
        }
    } else {
        s_restore_raw = pressed;
        s_restore_count = 1;
    }
    if (pressed && !s_restore_held && s_restore_count >= INPUT_DEBOUNCE_SAMPLES) {
        s_restore_held = 1;
        emit_key(0, INPUT_MOD_RESTORE, 1);
    } else if (!pressed && s_restore_held &&
               s_restore_count >= INPUT_DEBOUNCE_SAMPLES) {
        s_restore_held = 0;
        emit_key(0, 0, 0);
    }
}

/* ---------------------------------------------------------------- */

void input_init(void) {
    input_queue_init(&g_system_state.input);
    memset(s_raw, 0, sizeof(s_raw));
    memset(s_held, 0, sizeof(s_held));
    memset(s_count, 0, sizeof(s_count));
    memset(s_joy, 0, sizeof(s_joy));
    s_restore_raw = 0;
    s_restore_held = 0;
    s_restore_count = 0;
    s_mods = 0;
    proto_parser_init(&s_rs232_parser);
}

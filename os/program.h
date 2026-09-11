/* program.h
 *
 * Process model (Phase 5): a stack of Lua programs, each with its own
 * lua_State (capped heap), video state, timer list and input event
 * ring. The scheduler loop (driven from core 1) drains core 0's input
 * queue into the top program, runs due timers, otherwise calls tick().
 *
 * Ticks and timer callbacks never overlap: one scheduler step runs
 * either the due timers or one tick. A throwing tick or timer callback
 * terminates the program (finish() runs) and the parent resumes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lua.h"
#include "system_state.h"
#include "audio.h"
#include "video.h"

#define PROGRAM_MAX 4
#define PROGRAM_HEAP_CAP (64u * 1024u)
#define PROGRAM_TIMER_MAX 8
#define PROGRAM_EVENT_DEPTH 128

typedef struct {
    bool used;
    bool oneshot;
    uint32_t interval_ms;
    uint64_t deadline_us; /* absolute (os_time_us) */
    int fn_ref;           /* Lua registry ref to the callback */
} program_timer_t;

typedef struct program_s {
    /* -- lifetime -- */
    bool used;
    uint32_t pid;
    lua_State *L;
    int chunk_ref;     /* registry ref to the program chunk (kept alive) */
    int setup_ref;     /* LUA_NOREF when absent */
    int tick_ref;
    int finish_ref;
    bool exit_requested;

    /* -- heap budget -- */
    size_t heap_used;
    size_t heap_cap;

    /* -- video state (heap-allocated; pointer-swap save/restore) -- */
    video_state_t *video;

    /* -- audio state (heap-allocated like video) -- */
    audio_state_t *audio;

    /* -- timers -- */
    program_timer_t timers[PROGRAM_TIMER_MAX];
    bool paused;
    uint64_t pause_start_us; /* valid while paused */

    /* -- input event ring (core 1 only, no atomics needed) -- */
    input_event_t events[PROGRAM_EVENT_DEPTH];
    uint32_t ev_head; /* push here */
    uint32_t ev_tail; /* pop here */
    uint8_t joy[2];   /* current joystick bitmasks (INPUT_DIR_*) */

    struct program_s *next; /* parent program below on the stack */
} program_t;

/* One-time init (zeroes the pool). */
void program_init(void);

/* Boot the first program (the shell). Returns false on load failure.
 * `arg` (optional) is passed to the program chunk as its first vararg. */
bool program_boot(const char *path, const char *arg);

/* One scheduler iteration: drain input, run due timers, else tick. */
void program_scheduler_step(void);

/* Current top program (NULL when the stack is empty). */
program_t *program_top(void);

/* Launch `path` as a new program, pausing the current one. Returns 0
 * on success; on failure writes a message to *err and the current
 * program keeps running. `arg` (optional) is passed to the program
 * chunk as its first vararg. */
int program_launch(const char *path, const char *arg, const char **err);

/* Ask the current program to exit after its tick returns. */
void program_exit_request(void);

/* Call the given registry-ref function under pcall. Returns false on
 * a Lua error (message pushed on the stack). */
bool program_pcall(program_t *p, int fn_ref);

/* Terminate `p` (run finish(), close state, resume parent). */
void program_terminate(program_t *p);

/* Input event ring (core 1 only). */
void program_event_push(program_t *p, const input_event_t *ev);
bool program_event_pop(program_t *p, input_event_t *ev);

/* Timers. Returns the timer id (>= 1) or 0 when the list is full. */
int program_timer_create(program_t *p, int fn_ref, uint32_t interval_ms,
                         bool oneshot);
bool program_timer_stop(program_t *p, int id);

/* Fetch `path` from the SD card into a malloc'd buffer (via RPC).
 * Returns 0 on success; on failure *err gets a static message. */
int program_read_file(const char *path, char **out, size_t *out_len,
                      const char **err);

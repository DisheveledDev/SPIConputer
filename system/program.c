/* program.c
 *
 * Process model (Phase 5). See program.h for the model. Platform-
 * neutral: all time comes from os_time_us() and all SD access from
 * fs_lua_readall() (RPC), so the host tests run this exact code.
 */
#include "program.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"

#include "ff.h"
#include "f_util.h"

#include "fs_lua.h"
#include "os_time.h"
#include "sound_lua.h"
#include "sys_lua.h"
#include "screen_lua.h"

static program_t s_pool[PROGRAM_MAX];
static program_t *s_top = NULL;
static uint32_t s_next_pid = 0;

/* ------------------------------------------------------------------ */
/* Capped per-program heap                                             */
/* ------------------------------------------------------------------ */

/* Capped per-program heap.
 *
 * Lua's allocator contract (see lua.h): for a new block
 * frealloc(ud, NULL, tag, size) passes a *tag* as `osize`, not a size;
 * for realloc/free `osize` is the real old size. Only nsize bytes are
 * accounted on allocation, so the tag must not be subtracted here. */
static void *capped_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    program_t *p = (program_t *)ud;

    if (nsize == 0) { /* free: osize is the real size */
        free(ptr);
        p->heap_used -= osize;
        return NULL;
    }
    if (ptr == NULL) { /* new block: osize is a tag, ignore it */
        if (p->heap_used + nsize > p->heap_cap) {
            return NULL; /* budget exceeded */
        }
        void *np = malloc(nsize);
        if (!np) {
            return NULL;
        }
        p->heap_used += nsize;
        return np;
    }
    /* realloc: osize is the real old size */
    if (p->heap_used - osize + nsize > p->heap_cap) {
        return NULL; /* budget exceeded */
    }
    void *np = realloc(ptr, nsize);
    if (!np) {
        return NULL;
    }
    p->heap_used = p->heap_used - osize + nsize;
    return np;
}

/* ------------------------------------------------------------------ */
/* Pool and stack management                                           */
/* ------------------------------------------------------------------ */

void program_init(void) {
    memset(s_pool, 0, sizeof(s_pool));
    s_top = NULL;
    s_next_pid = 0;
    g_current_video = NULL;
    g_current_audio = NULL;
}

program_t *program_top(void) {
    return s_top;
}

static program_t *pool_alloc(void) {
    for (int i = 0; i < PROGRAM_MAX; i++) {
        if (!s_pool[i].used) {
            memset(&s_pool[i], 0, sizeof(s_pool[i]));
            s_pool[i].used = true;
            return &s_pool[i];
        }
    }
    return NULL;
}

static void pool_free(program_t *p) {
    memset(p, 0, sizeof(*p));
}

/* Shift a program's timer deadlines by a delta (resume after pause). */
static void timers_shift(program_t *p, uint64_t delta_us) {
    for (int i = 0; i < PROGRAM_TIMER_MAX; i++) {
        if (p->timers[i].used) {
            p->timers[i].deadline_us += delta_us;
        }
    }
}

static void program_pause(program_t *p) {
    p->paused = true;
    p->pause_start_us = os_time_us();
    /* Silence the program without losing its score position. */
    if (p->audio) audio_pause(p->audio);
}

static void program_resume(program_t *p) {
    uint64_t now = os_time_us();
    timers_shift(p, now - p->pause_start_us);
    p->paused = false;
}

/* ------------------------------------------------------------------ */
/* File loading (via the core 0 RPC)                                   */
/* ------------------------------------------------------------------ */

int program_read_file(const char *path, char **out, size_t *out_len,
                      char *resolved, size_t resolved_size,
                      const char **err) {
    FRESULT fr = fs_lua_read_program(path, out, out_len, resolved,
                                     resolved_size);
    if (fr != FR_OK) {
        *err = FRESULT_str(fr);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Program creation / launch / exit                                    */
/* ------------------------------------------------------------------ */

/* Create a program from a file (`source == NULL`) or a source string.
 * The global `args` table is set up before the chunk body runs, and
 * argv[0] (if any) is passed as the chunk's first vararg. */
static program_t *program_create(const char *name, const char *source,
                              size_t source_len, const char *const *argv,
                              int argc, const char **err) {
    program_t *p = pool_alloc();
    if (!p) {
        *err = "too many programs";
        return NULL;
    }

    p->heap_cap = PROGRAM_HEAP_CAP;
    p->L = lua_newstate(capped_alloc, p, 0);
    if (!p->L) {
        pool_free(p);
        *err = "out of memory";
        return NULL;
    }
    p->setup_ref = p->tick_ref = p->finish_ref = LUA_NOREF;
    p->on_keypress_ref = p->on_control_ref = LUA_NOREF;
    p->chunk_ref = LUA_NOREF;

    fs_lua_openlibs(p->L);
    sys_lua_openlibs(p->L);
    screen_lua_openlibs(p->L);
    sound_lua_openlibs(p->L);

    /* Back-reference so sys functions can find their program. */
    lua_pushlightuserdata(p->L, p);
    lua_setfield(p->L, LUA_REGISTRYINDEX, "_spi_program");

    /* Load the program: a file from the SD card (a source chunk or a
     * compiled `.prg`), or a source string. */
    char *buf = NULL;
    const char *chunk = source;
    const char *chunk_name = name;
    size_t len = source_len;
    char resolved[FS_LUA_PATH_MAX];
    if (!chunk) {
        if (program_read_file(name, &buf, &len, resolved, sizeof(resolved),
                              err) != 0) {
            lua_close(p->L);
            pool_free(p);
            return NULL;
        }
        chunk = buf;
        chunk_name = resolved;
    }
    int st = luaL_loadbufferx(p->L, chunk, len, chunk_name, "bt");
    free(buf);
    if (st != LUA_OK) {
        fprintf(stderr, "program_create: load error: %s\n",
                lua_tostring(p->L, -1));
        *err = lua_tostring(p->L, -1);
        lua_close(p->L);
        pool_free(p);
        return NULL;
    }

    /* Global `args` table (1-based), visible to the whole program. */
    lua_createtable(p->L, argc, 0);
    for (int i = 0; i < argc; i++) {
        lua_pushstring(p->L, argv[i]);
        lua_rawseti(p->L, -2, i + 1);
    }
    lua_setglobal(p->L, "args");

    /* Keep the chunk alive in the registry and run its body once:
     * the body defines setup()/tick()/finish(). The first launch
     * argument (if any) arrives as the chunk's first vararg. */
    p->chunk_ref = luaL_ref(p->L, LUA_REGISTRYINDEX);
    lua_rawgeti(p->L, LUA_REGISTRYINDEX, p->chunk_ref);
    if (argc > 0) {
        lua_pushstring(p->L, argv[0]);
    }
    st = lua_pcall(p->L, argc > 0 ? 1 : 0, 0, 0);
    if (st != LUA_OK) {
        fprintf(stderr, "program_create: chunk error: %s\n",
                lua_tostring(p->L, -1));
        *err = lua_tostring(p->L, -1);
        lua_close(p->L);
        pool_free(p);
        return NULL;
    }

    lua_getglobal(p->L, "__spi_interactive");
    p->interactive = !lua_isboolean(p->L, -1) || lua_toboolean(p->L, -1);
    lua_pop(p->L, 1);

    /* Collect the entry points (each optional except tick). */
    lua_getglobal(p->L, "setup");
    if (lua_isfunction(p->L, -1)) p->setup_ref = luaL_ref(p->L, LUA_REGISTRYINDEX);
    else lua_pop(p->L, 1);
    lua_getglobal(p->L, "tick");
    if (lua_isfunction(p->L, -1)) p->tick_ref = luaL_ref(p->L, LUA_REGISTRYINDEX);
    else lua_pop(p->L, 1);
    lua_getglobal(p->L, "finish");
    if (lua_isfunction(p->L, -1)) p->finish_ref = luaL_ref(p->L, LUA_REGISTRYINDEX);
    else lua_pop(p->L, 1);
    lua_getglobal(p->L, "on_keypress");
    if (lua_isfunction(p->L, -1)) p->on_keypress_ref = luaL_ref(p->L, LUA_REGISTRYINDEX);
    else lua_pop(p->L, 1);
    lua_getglobal(p->L, "on_control");
    if (lua_isfunction(p->L, -1)) p->on_control_ref = luaL_ref(p->L, LUA_REGISTRYINDEX);
    else lua_pop(p->L, 1);

    p->pid = s_next_pid++;
    lua_pushinteger(p->L, (lua_Integer)p->pid);
    lua_setglobal(p->L, "pid");

    if (p->interactive) {
        p->video = (video_state_t *)malloc(sizeof(video_state_t));
        if (!p->video) {
            lua_close(p->L);
            pool_free(p);
            *err = "out of memory";
            return NULL;
        }
        video_state_init(p->video);
        p->audio = (audio_state_t *)malloc(sizeof(audio_state_t));
        if (!p->audio) {
            video_state_free(p->video);
            free(p->video);
            lua_close(p->L);
            pool_free(p);
            *err = "out of memory";
            return NULL;
        }
        audio_state_init(p->audio);
    }
    return p;
}

bool program_pcall(program_t *p, int fn_ref) {
    lua_settop(p->L, 0);
    lua_rawgeti(p->L, LUA_REGISTRYINDEX, fn_ref);
    return lua_pcall(p->L, 0, 0, 0) == LUA_OK;
}

/* Pop `p` from the stack (p must be the top) and resume the parent. */
static void program_pop(program_t *p) {
    s_top = p->next;
    if (s_top) {
        program_resume(s_top);
    }
    g_current_video = (s_top && s_top->interactive) ? s_top->video : NULL;
    g_current_audio = (s_top && s_top->interactive) ? s_top->audio : NULL;
}

void program_terminate(program_t *p) {
    if (p != s_top) {
        return;
    }
    if (!p->interactive && p->next) {
        p->next->child_result_pending = true;
        p->next->child_result_ok = p->utility_result_set && p->utility_ok;
        snprintf(p->next->child_result_output,
                 sizeof(p->next->child_result_output), "%s",
                 p->utility_result_set ? p->utility_output : "utility exited without a result");
    }
    /* finish() runs for any program that completed setup(). */
    if (p->finish_ref != LUA_NOREF) {
        if (!program_pcall(p, p->finish_ref)) {
            fprintf(stderr, "program %u: finish error: %s\n", p->pid,
                    lua_tostring(p->L, -1));
        }
    }
    lua_close(p->L);
    if (p->video) {
        video_state_free(p->video);
        free(p->video);
    }
    if (p->audio) {
        audio_state_free(p->audio);
        free(p->audio);
    }
    program_pop(p); /* reads p->next: must run before pool_free zeroes it */
    pool_free(p);
}

static void run_setup(program_t *p, const char **err) {
    if (p->setup_ref == LUA_NOREF) {
        return;
    }
    if (!program_pcall(p, p->setup_ref)) {
        fprintf(stderr, "program_create: setup error: %s\n",
                lua_tostring(p->L, -1));
        *err = lua_tostring(p->L, -1);
    }
}

static int launch_common(const char *name, const char *source, size_t len,
                         const char *const *argv, int argc,
                         const char **err) {
    *err = NULL;
    if (argc > PROGRAM_ARG_MAX) {
        *err = "too many arguments";
        return -1;
    }
    if (s_top && s_top->interactive && s_top->video->mode == VIDEO_MODE_PIXEL) {
        /* Memory policy: mode 10 is single-program (see AGENTS.md). */
        *err = "cannot launch from mode 10";
        return -1;
    }
    if (s_top) {
        program_pause(s_top);
    }
    program_t *p = program_create(name, source, len, argv, argc, err);
    if (!p) {
        if (s_top) {
            program_resume(s_top);
        }
        return -1;
    }
    p->next = s_top;
    s_top = p;
    if (p->interactive) {
        g_current_video = p->video;
        g_current_audio = p->audio;
    }

    run_setup(p, err);
    if (*err) {
        /* Setup failed: the program never started; close it without
         * finish() and resume the parent. */
        lua_close(p->L);
        if (p->video) {
            video_state_free(p->video);
            free(p->video);
        }
        if (p->audio) {
            audio_state_free(p->audio);
            free(p->audio);
        }
        program_pop(p); /* reads p->next before pool_free zeroes it */
        pool_free(p);
        return -1;
    }
    return 0;
}

int program_launch(const char *path, const char *arg, const char **err) {
    const char *argv[1];
    int argc = 0;
    if (arg) {
        argv[0] = arg;
        argc = 1;
    }
    return launch_common(path, NULL, 0, argc ? argv : NULL, argc, err);
}

int program_launch_args(const char *path, const char *const *argv, int argc,
                        const char **err) {
    return launch_common(path, NULL, 0, argv, argc, err);
}

int program_launch_source(const char *name, const char *source, size_t len,
                          const char *const *argv, int argc,
                          const char **err) {
    return launch_common(name, source, len, argv, argc, err);
}

bool program_boot(const char *path, const char *arg) {
    program_init();
    const char *err = NULL;
    if (program_launch(path, arg, &err) != 0) {
        fprintf(stderr, "boot: %s: %s\n", path, err ? err : "unknown error");
        return false;
    }
    return true;
}

void program_exit_request(void) {
    if (s_top) {
        s_top->exit_requested = true;
    }
}

bool program_utility_result(program_t *p, bool *ok, const char **output) {
    if (!p || p->interactive || !p->utility_result_set) {
        return false;
    }
    if (ok) *ok = p->utility_ok;
    if (output) *output = p->utility_output;
    p->utility_result_set = false;
    return true;
}

/* ------------------------------------------------------------------ */
/* Input event deposit (per-program ring, core 1 only)                 */
/* ------------------------------------------------------------------ */

void program_event_push(program_t *p, const input_event_t *ev) {
    uint32_t next = (p->ev_head + 1) % PROGRAM_EVENT_DEPTH;
    if (next == p->ev_tail) {
        p->ev_tail = (p->ev_tail + 1) % PROGRAM_EVENT_DEPTH; /* drop oldest */
    }
    p->events[p->ev_head] = *ev;
    p->ev_head = next;

    if (ev->type == INPUT_EV_CONTROL1) {
        uint8_t bit = ev->dirs;
        if (ev->pressed) p->joy[0] |= bit;
        else p->joy[0] &= (uint8_t)~bit;
    } else if (ev->type == INPUT_EV_CONTROL2) {
        uint8_t bit = ev->dirs;
        if (ev->pressed) p->joy[1] |= bit;
        else p->joy[1] &= (uint8_t)~bit;
    }
}

bool program_event_pop(program_t *p, input_event_t *ev) {
    if (p->ev_head == p->ev_tail) {
        return false;
    }
    *ev = p->events[p->ev_tail];
    p->ev_tail = (p->ev_tail + 1) % PROGRAM_EVENT_DEPTH;
    return true;
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

int program_timer_create(program_t *p, int fn_ref, uint32_t interval_ms,
                         bool oneshot) {
    for (int i = 0; i < PROGRAM_TIMER_MAX; i++) {
        if (!p->timers[i].used) {
            p->timers[i].used = true;
            p->timers[i].oneshot = oneshot;
            p->timers[i].interval_ms = interval_ms;
            p->timers[i].deadline_us =
                os_time_us() + (uint64_t)interval_ms * 1000;
            p->timers[i].fn_ref = fn_ref;
            return i + 1; /* timer id */
        }
    }
    return 0; /* no free slot */
}

bool program_timer_stop(program_t *p, int id) {
    if (id < 1 || id > PROGRAM_TIMER_MAX || !p->timers[id - 1].used) {
        return false;
    }
    luaL_unref(p->L, LUA_REGISTRYINDEX, p->timers[id - 1].fn_ref);
    p->timers[id - 1].fn_ref = LUA_NOREF;
    p->timers[id - 1].used = false;
    return true;
}

/* Run one due timer callback. Returns false if the program died. */
static bool run_timer(program_t *p, program_timer_t *t) {
    if (!program_pcall(p, t->fn_ref)) {
        fprintf(stderr, "program %u: timer error: %s\n", p->pid,
                lua_tostring(p->L, -1));
        program_terminate(p);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Optional input callbacks (on_keypress / on_control)                 */
/* ------------------------------------------------------------------ */

static bool callback_failed(program_t *p, const char *what) {
    fprintf(stderr, "program %u: %s error: %s\n", p->pid, what,
            lua_tostring(p->L, -1));
    program_terminate(p);
    return false;
}

/* on_keypress(key, shift, ctrl, cbm, restore) on key-down events. */
static bool call_on_keypress(program_t *p, const input_event_t *ev) {
    lua_State *L = p->L;
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, p->on_keypress_ref);
    lua_pushinteger(L, ev->key);
    lua_pushboolean(L, (ev->mods & INPUT_MOD_SHIFT) != 0);
    lua_pushboolean(L, (ev->mods & INPUT_MOD_CTRL) != 0);
    lua_pushboolean(L, (ev->mods & INPUT_MOD_CBM) != 0);
    lua_pushboolean(L, (ev->mods & INPUT_MOD_RESTORE) != 0);
    if (lua_pcall(L, 5, 0, 0) != LUA_OK) {
        return callback_failed(p, "on_keypress");
    }
    return true;
}

/* on_control(index, up, down, left, right, fire) with the full state
 * after the event; index is the port (0 = joystick 1, 1 = joystick 2). */
static bool call_on_control(program_t *p, int index, uint8_t dirs) {
    lua_State *L = p->L;
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, p->on_control_ref);
    lua_pushinteger(L, index);
    lua_pushboolean(L, (dirs & INPUT_DIR_UP) != 0);
    lua_pushboolean(L, (dirs & INPUT_DIR_DOWN) != 0);
    lua_pushboolean(L, (dirs & INPUT_DIR_LEFT) != 0);
    lua_pushboolean(L, (dirs & INPUT_DIR_RIGHT) != 0);
    lua_pushboolean(L, (dirs & INPUT_DIR_FIRE) != 0);
    if (lua_pcall(L, 6, 0, 0) != LUA_OK) {
        return callback_failed(p, "on_control");
    }
    return true;
}

/* Returns false when a callback error terminated the program. Events
 * also stay in the program's ring, so InputPoll() still sees them. */
static bool dispatch_input_callbacks(program_t *p, const input_event_t *ev) {
    if (ev->type == INPUT_EV_KEY) {
        if (p->on_keypress_ref == LUA_NOREF) {
            return true;
        }
        /* Presses only: releases and modifier-key events (key 0) are
         * available through InputPoll(). */
        if (ev->pressed == 0 || ev->key == 0) {
            return true;
        }
        return call_on_keypress(p, ev);
    }
    if (ev->type == INPUT_EV_CONTROL1 || ev->type == INPUT_EV_CONTROL2) {
        if (p->on_control_ref == LUA_NOREF) {
            return true;
        }
        int index = ev->type == INPUT_EV_CONTROL1 ? 0 : 1;
        return call_on_control(p, index, p->joy[index]);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Scheduler                                                           */
/* ------------------------------------------------------------------ */

void program_scheduler_step(void) {
    program_t *p = s_top;
    if (!p) {
        return;
    }

    /* 1. Drain core 0's input queue into the top program's ring, then run
     *    the optional input callbacks; events stay available to
     *    InputPoll() either way. */
    input_event_t ev;
    while (input_queue_pop(&g_system_state.input, &ev)) {
        program_event_push(p, &ev);
        if (!dispatch_input_callbacks(p, &ev)) {
            return; /* program terminated by a callback error */
        }
    }

    uint64_t now = os_time_us();

    /* 2. Run due timers (one batch per step; no tick on timer steps). */
    bool ran_timer = false;
    for (int i = 0; i < PROGRAM_TIMER_MAX; i++) {
        program_timer_t *t = &p->timers[i];
        if (!t->used || t->deadline_us > now) {
            continue;
        }
        int fn_ref = t->fn_ref; /* callback may stop/recreate this slot */
        ran_timer = true;
        if (!run_timer(p, t)) {
            return; /* program terminated */
        }
        if (p->exit_requested) {
            break;
        }
        if (t->fn_ref != fn_ref || !t->used) {
            continue; /* slot was stopped or recreated inside the callback */
        }
        if (!t->oneshot) {
            t->deadline_us += (uint64_t)t->interval_ms * 1000;
            if (t->deadline_us <= now) {
                /* Missed while away: skip, no catch-up bursts. */
                t->deadline_us = now + (uint64_t)t->interval_ms * 1000;
            }
        } else {
            luaL_unref(p->L, LUA_REGISTRYINDEX, t->fn_ref);
            t->used = false;
        }
    }
    if (ran_timer) {
        if (p->exit_requested && p == s_top) {
            program_terminate(p);
        }
        return;
    }

    /* 3. Otherwise: tick(). */
    if (p->tick_ref == LUA_NOREF) {
        if (p->exit_requested && p == s_top) {
            program_terminate(p);
        }
        return;
    }
    if (!program_pcall(p, p->tick_ref)) {
        fprintf(stderr, "program %u: tick error: %s\n", p->pid,
                lua_tostring(p->L, -1));
        program_terminate(p);
        return;
    }
    if (p->exit_requested && p == s_top) {
        program_terminate(p);
    }
}

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

#include "fs_core0.h"
#include "fs_lua.h"
#include "input.h"
#include "os_time.h"
#include "system_state.h"
#include "sound_lua.h"
#include "sys_lua.h"
#include "screen_lua.h"

static program_t s_pool[PROGRAM_MAX];
_Static_assert(PROGRAM_MAX <= VIDEO_SLOTS,
               "every program needs its own screen slot (video.h)");
static program_t *s_top = NULL;
static uint32_t s_next_pid = 0;

static void program_log_event(const program_t *p, const char *event,
                              const char *detail) {
    char line[224];
    snprintf(line, sizeof(line), "program: t=%llu pid=%lu name=%s event=%s%s%s",
             (unsigned long long)os_time_us(), (unsigned long)p->pid, p->name,
             event, detail ? " detail=" : "", detail ? detail : "");
    fs_core0_debug_log(line);
}

static void program_log_error(const program_t *p, const char *phase,
                              const char *message) {
    const char *base = strrchr(p->name, '/');
    base = base ? base + 1 : p->name;
    char name[64];
    size_t n = 0;
    while (base[n] && base[n] != '.' && n + 1 < sizeof(name)) {
        unsigned char c = (unsigned char)base[n];
        name[n] = (c == '_' || (c >= '0' && c <= '9') ||
                   (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                      ? (char)c
                      : '_';
        n++;
    }
    name[n] = '\0';
    if (n == 0) {
        snprintf(name, sizeof(name), "program");
    }
    uint64_t timestamp = os_time_us();
    luaL_traceback(p->L, p->L, message ? message : "unknown Lua error", 1);
    const char *traceback = lua_tostring(p->L, -1);
    char detail[2048];
    snprintf(detail, sizeof(detail),
             "program=%s\npid=%lu\nphase=%s\ntimestamp_us=%llu\n"
             "source=%s\nerror=%s\ntraceback=%s\n",
             name, (unsigned long)p->pid, phase,
             (unsigned long long)timestamp, p->name,
             message ? message : "unknown Lua error",
             traceback ? traceback : "unavailable");
    fs_core0_write_error(name, timestamp, detail);
}

/* Deferred audio state frees.
 *
 * A program pop can land mid-frame; the audio path may still be reading
 * the state, so terminated states are parked here for two frame
 * boundaries instead. */
#define PROGRAM_RETIRE_MAX PROGRAM_MAX

typedef struct {
    bool used;
    uint32_t when; /* free once the frame counter reaches this */
    audio_state_t *audio;
} program_retire_t;

static program_retire_t s_retire[PROGRAM_RETIRE_MAX];

static void program_retire(audio_state_t *audio) {
    if (!audio) {
        return;
    }
    for (int i = 0; i < PROGRAM_RETIRE_MAX; i++) {
        if (!s_retire[i].used) {
            s_retire[i].used = true;
            s_retire[i].when = g_system_state.video_frame_count + 2;
            s_retire[i].audio = audio;
            return;
        }
    }
    /* Queue full (cannot happen with one pop per frame): free now. */
    audio_state_free(audio);
    free(audio);
}

void program_retire_reap(uint32_t frame_count) {
    for (int i = 0; i < PROGRAM_RETIRE_MAX; i++) {
        program_retire_t *r = &s_retire[i];
        if (!r->used || (int32_t)(frame_count - r->when) < 0) {
            continue;
        }
        if (r->audio) {
            audio_state_free(r->audio);
            free(r->audio);
        }
        r->used = false;
    }
}

int program_retire_pending(void) {
    int n = 0;
    for (int i = 0; i < PROGRAM_RETIRE_MAX; i++) {
        n += s_retire[i].used;
    }
    return n;
}

/* Programs replaced by a Launch(..., replace): the replaced program is
 * still inside its own Lua call when the handover happens, so its state
 * cannot be closed there. Park it and release it at the start of the
 * next scheduler step (finish() still runs: the handover was
 * deliberate). */
#define PROGRAM_REPLACED_MAX PROGRAM_MAX
static program_t *s_replaced[PROGRAM_REPLACED_MAX];
static int s_replaced_count;

static void program_defer_replaced(program_t *p) {
    if (s_replaced_count >= PROGRAM_REPLACED_MAX) {
        /* Unreachable: a replace needs a running program, and at most
         * PROGRAM_MAX exist. Leak rather than close a live state. */
        fprintf(stderr, "program %u: replaced program was not reaped\n",
                p->pid);
        return;
    }
    s_replaced[s_replaced_count++] = p;
}

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
    memset(s_retire, 0, sizeof(s_retire));
    s_top = NULL;
    s_next_pid = 0;
    s_replaced_count = 0;
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
    if (p->requires_audio && p->audio) audio_pause(p->audio);
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
static void parent_directory(const char *path, char *out, size_t size) {
    snprintf(out, size, "%s", path);
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) {
        snprintf(out, size, "/");
    } else {
        *slash = '\0';
    }
}

static void resolve_program_path(const char *path, const char *base,
                                 char *out, size_t size) {
    if (path[0] == '/' || !base || !base[0] || strcmp(base, "/") == 0) {
        snprintf(out, size, "%s", path);
    } else {
        snprintf(out, size, "%s/%s", base, path);
    }
}

static program_t *program_create(const char *name, const char *source,
                              size_t source_len, const char *const *argv,
                              int argc, const char **err) {
    program_t *p = pool_alloc();
    if (!p) {
        *err = "too many programs";
        return NULL;
    }

    snprintf(p->name, sizeof(p->name), "%s", name);
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
    parent_directory(name, p->cwd, sizeof(p->cwd));
    lua_pushstring(p->L, p->cwd);
    lua_setfield(p->L, LUA_REGISTRYINDEX, "_spi_cwd");

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
        char load_log[320];
        snprintf(load_log, sizeof(load_log),
                 "loader: requested=%s resolved=%s bytes=%lu", name,
                 chunk_name, (unsigned long)len);
        fs_core0_debug_log(load_log);
    }
    int st = luaL_loadbufferx(p->L, chunk, len, chunk_name, "bt");
    free(buf);
    if (st != LUA_OK) {
        const char *message = lua_tostring(p->L, -1);
        fprintf(stderr, "program_create: load error: %s\n", message);
        program_log_error(p, "compile", message);
        *err = message;
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

    char app_root[FS_LUA_PATH_MAX];
    snprintf(app_root, sizeof(app_root), "%s", chunk_name);
    char *slash = strrchr(app_root, '/');
    if (slash) {
        *slash = '\0';
    } else {
        snprintf(app_root, sizeof(app_root), ".");
    }
    lua_pushstring(p->L, app_root);
    lua_setfield(p->L, LUA_REGISTRYINDEX, "_spi_app_root");
    lua_newtable(p->L);
    lua_pushstring(p->L, chunk_name);
    lua_setfield(p->L, -2, "program");
    lua_pushstring(p->L, app_root);
    lua_setfield(p->L, -2, "root");
    char resource_root[FS_LUA_PATH_MAX];
    snprintf(resource_root, sizeof(resource_root), "%s/resources", app_root);
    lua_pushstring(p->L, resource_root);
    lua_setfield(p->L, -2, "resources");
    char metadata_path[FS_LUA_PATH_MAX];
    snprintf(metadata_path, sizeof(metadata_path), "%s/app.json", app_root);
    lua_pushstring(p->L, metadata_path);
    lua_setfield(p->L, -2, "metadata");
    lua_setglobal(p->L, "app");

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
        const char *message = lua_tostring(p->L, -1);
        fprintf(stderr, "program_create: chunk error: %s\n", message);
        program_log_error(p, "runtime", message);
        *err = message;
        lua_close(p->L);
        pool_free(p);
        return NULL;
    }
    program_log_event(p, "chunk-ok", chunk_name);

    lua_getglobal(p->L, "__spi_interactive");
    p->interactive = !lua_isboolean(p->L, -1) || lua_toboolean(p->L, -1);
    lua_pop(p->L, 1);
    lua_getglobal(p->L, "__spi_requires_video");
    p->requires_video = p->interactive &&
                        (!lua_isboolean(p->L, -1) || lua_toboolean(p->L, -1));
    lua_pop(p->L, 1);
    lua_getglobal(p->L, "__spi_requires_audio");
    p->requires_audio = p->interactive &&
                        (!lua_isboolean(p->L, -1) || lua_toboolean(p->L, -1));
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
    p->vsync_last_frames = g_system_state.video_frame_count;

    if (p->requires_audio) {
        p->audio = (audio_state_t *)malloc(sizeof(audio_state_t));
        if (!p->audio) {
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

/* Point core 0 at the screen slot a program owns: its pool index. The
 * display state itself never leaves core 0; this only queues a slot
 * select (and, on launch, the reset that clears the slot). */
static void program_select_screen(program_t *p, bool reset) {
    if (!p || !p->interactive || !p->requires_video) {
        return;
    }
    video_op_t slot = {.op = VIDEO_OP_SLOT, .a = (uint8_t)(p - s_pool)};
    video_op_put(&slot);
    if (reset) {
        video_op_t op = {.op = VIDEO_OP_RESET};
        video_op_put(&op);
        video_note_mode(VIDEO_MODE_TEXT40);
    }
}

/* Pop `p` from the stack (p must be the top) and resume the parent. */
static void program_pop(program_t *p) {
    s_top = p->next;
    if (s_top) {
        program_resume(s_top);
    }
    program_select_screen(s_top, false);
    g_current_audio = (s_top && s_top->requires_audio) ? s_top->audio : NULL;
}

void program_terminate(program_t *p) {
    if (p != s_top) {
        return;
    }
    program_log_event(p, "terminate", p->exit_requested ? "requested" : "error");
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
            const char *message = lua_tostring(p->L, -1);
            fprintf(stderr, "program %u: finish error: %s\n", p->pid,
                    message);
            program_log_error(p, "finish", message);
        }
    }
    lua_close(p->L);
    /* The audio path may still be reading the state; park it until two
     * frame boundaries have passed (see program_retire). */
    program_retire(p->audio);
    program_pop(p); /* reads p->next: must run before pool_free zeroes it */
    pool_free(p);
}

/* Release the programs a replace-launch parked (see program_defer_replaced).
 * Runs once per scheduler step, so the replaced program has returned from
 * its Lua call by the time its state is closed. */
static void program_reap_replaced(void) {
    for (int i = 0; i < s_replaced_count; i++) {
        program_t *p = s_replaced[i];
        if (p->finish_ref != LUA_NOREF) {
            if (!program_pcall(p, p->finish_ref)) {
                const char *message = lua_tostring(p->L, -1);
                fprintf(stderr, "program %u: finish error: %s\n", p->pid,
                        message);
                program_log_error(p, "finish", message);
            }
        }
        lua_close(p->L);
        program_retire(p->audio);
        pool_free(p);
    }
    s_replaced_count = 0;
}

static void run_setup(program_t *p, const char **err) {
    if (p->setup_ref == LUA_NOREF) {
        return;
    }
    if (!program_pcall(p, p->setup_ref)) {
        const char *message = lua_tostring(p->L, -1);
        fprintf(stderr, "program_create: setup error: %s\n", message);
        program_log_error(p, "setup", message);
        *err = message;
    }
}

static int launch_common(const char *name, const char *source, size_t len,
                         const char *const *argv, int argc, bool replace,
                         const char **err) {
    *err = NULL;
    if (argc > PROGRAM_ARG_MAX) {
        *err = "too many arguments";
        return -1;
    }
    if (!replace && s_top && s_top->requires_video &&
        video_lua_mode() == VIDEO_MODE_PIXEL) {
        /* Memory policy: mode 10 is single-program (see AGENTS.md).
         * Replacing the pixel-mode program is allowed: the new program
         * takes its place and the slot resets. */
        *err = "cannot launch from mode 10";
        return -1;
    }
    if (s_top) {
        program_pause(s_top);
    }
    char resolved_name[FS_LUA_PATH_MAX];
    const char *base = s_top ? s_top->cwd : "/";
    resolve_program_path(name, base, resolved_name, sizeof(resolved_name));
    program_t *p = program_create(source ? name : resolved_name, source, len,
                                  argv, argc, err);
    if (!p) {
        if (s_top) {
            program_resume(s_top);
        }
        return -1;
    }
    snprintf(p->cwd, sizeof(p->cwd), "%s", base && base[0] ? base : "/");
    lua_pushstring(p->L, p->cwd);
    lua_setfield(p->L, LUA_REGISTRYINDEX, "_spi_cwd");
    lua_getglobal(p->L, "app");
    if (lua_istable(p->L, -1)) {
        lua_pushstring(p->L, p->cwd);
        lua_setfield(p->L, -2, "cwd");
    }
    lua_pop(p->L, 1);
    if (replace && s_top) {
        /* Take the caller's place on the stack; the caller hands its
         * parent over to us and is released after its call returns. */
        program_t *old = s_top;
        p->next = old->next;
        s_top = p;
        program_defer_replaced(old);
    } else {
        p->next = s_top;
        s_top = p;
    }
    program_log_event(p, replace ? "replace-created" : "created", NULL);
    if (p->interactive) {
        program_select_screen(p, true);
        g_current_audio = p->audio;
    }

    run_setup(p, err);
    if (*err) {
        /* Setup failed: the program never started; close it without
         * finish() and resume the parent. */
        lua_close(p->L);
        if (p->audio) {
            audio_state_free(p->audio);
            free(p->audio);
        }
        program_pop(p); /* reads p->next before pool_free zeroes it */
        pool_free(p);
        return -1;
    }
    program_log_event(p, "setup-ok", NULL);
    return 0;
}

int program_launch(const char *path, const char *arg, const char **err) {
    const char *argv[1];
    int argc = 0;
    if (arg) {
        argv[0] = arg;
        argc = 1;
    }
    return launch_common(path, NULL, 0, argc ? argv : NULL, argc, false,
                         err);
}

int program_launch_replace(const char *path, const char *arg,
                           const char **err) {
    const char *argv[1];
    int argc = 0;
    if (arg) {
        argv[0] = arg;
        argc = 1;
    }
    return launch_common(path, NULL, 0, argc ? argv : NULL, argc, true, err);
}

int program_launch_args(const char *path, const char *const *argv, int argc,
                        const char **err) {
    return launch_common(path, NULL, 0, argv, argc, false, err);
}

int program_launch_source(const char *name, const char *source, size_t len,
                          const char *const *argv, int argc,
                          const char **err) {
    return launch_common(name, source, len, argv, argc, false, err);
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
        const char *message = lua_tostring(p->L, -1);
        fprintf(stderr, "program %u: timer error: %s\n", p->pid,
                message);
        program_log_error(p, "timer", message);
        program_terminate(p);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Optional input callbacks (on_keypress / on_control)                 */
/* ------------------------------------------------------------------ */

static bool callback_failed(program_t *p, const char *what) {
    const char *message = lua_tostring(p->L, -1);
    fprintf(stderr, "program %u: %s error: %s\n", p->pid, what,
            message);
    program_log_error(p, what, message);
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

    /* Release any program a replace-launch handed over from (its Lua
     * call has returned by now), then reap audio states whose two-frame
     * grace period has passed (or, on the host, whose fake frame count
     * has). Both run even with an empty stack so nothing accumulates. */
    program_reap_replaced();
    program_retire_reap(g_system_state.video_frame_count);

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
        if (p != s_top) {
            return; /* a callback launched or replaced this program */
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
        if (p != s_top) {
            return; /* a timer covered or replaced this program */
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
    if (!p->scheduler_started) {
        p->scheduler_started = true;
        program_log_event(p, "tick-first", NULL);
    }
    if (!program_pcall(p, p->tick_ref)) {
        const char *message = lua_tostring(p->L, -1);
        fprintf(stderr, "program %u: tick error: %s\n", p->pid,
                message);
        program_log_error(p, "tick", message);
        program_terminate(p);
        return;
    }
    if (p != s_top) {
        return; /* tick launched a program or replaced this one */
    }
    if (p->exit_requested && p == s_top) {
        program_terminate(p);
    }
}

/* sys_lua.c
 *
 * OS services exposed to programs (Phase 5). All functions look up
 * their program via the "_spi_program" registry back-reference, so
 * they only work inside a program state.
 *
 *   TimeNow()                     -> milliseconds since boot
 *   Pid()                         -> this program's pid
 *   ExitProgram()                 -> request exit after the current tick
 *   Launch(path [, arg [, replace]]) -> true | nil, err (pauses the
 *                                    caller; with replace the caller
 *                                    leaves the stack and its state is
 *                                    released)
 *   Execute(path, args...)        -> true | nil, err (foreground run)
 *   ExecuteString(src, args...)   -> true | nil, err (run Lua source)
 *   TimerCreate(fn, ms [, oneshot]) -> timer id
 *   TimerStop(id)                 -> bool
 *   InputPoll()                   -> next event table | nil
 *   InputControl(n)               -> {up,down,left,right,fire} | nil
 *   WaitVSync([ms])               -> frames elapsed since this
 *                                    program's previous WaitVSync;
 *                                    blocks up to ms for a frame
 *   Compile(src [, dst])          -> true, bytes | nil, err: compile a
 *                                    Lua source file on the card to a
 *                                    .prg binary chunk next to it
 */
#include "sys_lua.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"

#include "ff.h"
#include "f_util.h"

#include "fs_lua.h"
#include "input.h"
#include "os_time.h"
#include "program.h"
#include "system_state.h"

static program_t *program_of(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "_spi_program");
    program_t *p = (program_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!p) {
        luaL_error(L, "OS function called outside a program");
    }
    return p;
}

/* ---------------- time / process ---------------- */

static int sys_timenow(lua_State *L) {
    (void)program_of(L);
    lua_pushinteger(L, (lua_Integer)(os_time_us() / 1000));
    return 1;
}

static int sys_pid(lua_State *L) {
    program_t *p = program_of(L);
    lua_pushinteger(L, (lua_Integer)p->pid);
    return 1;
}

static int sys_exit(lua_State *L) {
    (void)program_of(L);
    program_exit_request();
    return 0;
}

static int sys_launch(lua_State *L) {
    (void)program_of(L);
    const char *path = luaL_checkstring(L, 1);
    const char *arg = luaL_optstring(L, 2, NULL);
    int replace = lua_toboolean(L, 3);
    const char *err = NULL;
    int rc = replace ? program_launch_replace(path, arg, &err)
                     : program_launch(path, arg, &err);
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "launch failed");
        return 2;
    }
    /* Success: the caller is now paused (or, when replacing, on its way
     * out once this call returns). */
    lua_pushboolean(L, true);
    return 1;
}

/* Collect strings from indices [first, top] into argv. Returns the
 * count, or -1 when there are more than `max`. */
static int collect_args(lua_State *L, int first, const char **argv, int max) {
    int argc = 0;
    int top = lua_gettop(L);
    for (int i = first; i <= top; i++) {
        if (argc >= max) {
            return -1;
        }
        argv[argc++] = luaL_checkstring(L, i);
    }
    return argc;
}

/* Execute(path, args...): run a program file in the foreground; the
 * caller is paused until it exits. */
static int sys_execute(lua_State *L) {
    (void)program_of(L);
    const char *path = luaL_checkstring(L, 1);
    const char *argv[PROGRAM_ARG_MAX];
    int argc = collect_args(L, 2, argv, PROGRAM_ARG_MAX);
    if (argc < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many arguments");
        return 2;
    }
    const char *err = NULL;
    if (program_launch_args(path, argv, argc, &err) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "execute failed");
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

/* ExecuteString(source, args...): compile and run the string as a
 * program in the foreground, with the same semantics as Execute. */
static int sys_execute_string(lua_State *L) {
    (void)program_of(L);
    size_t len = 0;
    const char *source = luaL_checklstring(L, 1, &len);
    const char *argv[PROGRAM_ARG_MAX];
    int argc = collect_args(L, 2, argv, PROGRAM_ARG_MAX);
    if (argc < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many arguments");
        return 2;
    }
    const char *err = NULL;
    if (program_launch_source("ExecuteString", source, len, argv, argc,
                              &err) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "execute failed");
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

/* ---------------- timers ---------------- */

static int sys_timer_create(lua_State *L) {
    program_t *p = program_of(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_Integer interval = luaL_checkinteger(L, 2);
    int oneshot = lua_toboolean(L, 3);
    if (interval < 1) {
        return luaL_error(L, "timer interval must be >= 1 ms");
    }
    lua_pushvalue(L, 1);
    int fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int id = program_timer_create(p, fn_ref, (uint32_t)interval, oneshot != 0);
    if (id == 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, fn_ref);
        return luaL_error(L, "too many timers");
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

static int sys_timer_stop(lua_State *L) {
    program_t *p = program_of(L);
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, program_timer_stop(p, id));
    return 1;
}

/* ---------------- input ---------------- */

static void push_event_table(lua_State *L, const input_event_t *ev) {
    lua_newtable(L);
    if (ev->type == INPUT_EV_KEY) {
        lua_pushliteral(L, "key");
    } else if (ev->type == INPUT_EV_CONTROL1) {
        lua_pushliteral(L, "control1");
    } else {
        lua_pushliteral(L, "control2");
    }
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, ev->key);
    lua_setfield(L, -2, "key");
    lua_pushinteger(L, ev->mods);
    lua_setfield(L, -2, "mods");
    lua_pushinteger(L, ev->pressed);
    lua_setfield(L, -2, "pressed");
    lua_pushinteger(L, ev->ctrl);
    lua_setfield(L, -2, "ctrl");
    lua_pushinteger(L, ev->dirs);
    lua_setfield(L, -2, "dirs");
}

static int sys_input_poll(lua_State *L) {
    program_t *p = program_of(L);
    input_event_t ev;
    if (!program_event_pop(p, &ev)) {
        lua_pushnil(L);
        return 1;
    }
    push_event_table(L, &ev);
    return 1;
}

static int sys_input_control(lua_State *L) {
    program_t *p = program_of(L);
    lua_Integer n = luaL_checkinteger(L, 1);
    if (n < 1 || n > 2) {
        lua_pushnil(L);
        return 1;
    }
    uint8_t dirs = p->joy[n - 1];
    lua_newtable(L);
    lua_pushboolean(L, (dirs & INPUT_DIR_UP) != 0);
    lua_setfield(L, -2, "up");
    lua_pushboolean(L, (dirs & INPUT_DIR_DOWN) != 0);
    lua_setfield(L, -2, "down");
    lua_pushboolean(L, (dirs & INPUT_DIR_LEFT) != 0);
    lua_setfield(L, -2, "left");
    lua_pushboolean(L, (dirs & INPUT_DIR_RIGHT) != 0);
    lua_setfield(L, -2, "right");
    lua_pushboolean(L, (dirs & INPUT_DIR_FIRE) != 0);
    lua_setfield(L, -2, "fire");
    return 1;
}

/* WaitVSync([ms]) -> frames elapsed
 *
 * Blocks the running program (and only the running program: the
 * scheduler skips its ticks meanwhile) until core 0's scanout crosses
 * a frame boundary. Input events keep accumulating in the program
 * ring, so nothing is lost. `ms` bounds the wait; without it the
 * syscall gives up after about six frames so a display-less board or a
 * stalled scanout cannot hang a program forever. */
static int sys_wait_vsync(lua_State *L) {
    program_t *p = program_of(L);
    lua_Integer timeout_ms = 100;
    if (!lua_isnoneornil(L, 1)) {
        timeout_ms = luaL_checkinteger(L, 1);
    }

    uint32_t start = p->vsync_last_frames;
    uint64_t deadline = os_time_us() + (uint64_t)timeout_ms * 1000;
    uint32_t frames;

    /* Read the frame counter at least once, so WaitVSync(0) reports the
     * frames elapsed since this program's previous call instead of
     * always returning zero. Input events keep accumulating in the
     * program's ring while the program blocks here; nothing is lost. */
    do {
        atomic_signal_fence(memory_order_seq_cst);
        frames = g_system_state.video_frame_count;
        if (frames != start) {
            break;
        }
    } while (os_time_us() < deadline);

    p->vsync_last_frames = frames;
    lua_pushinteger(L, (lua_Integer)(frames - start));
    return 1;
}

/* ---------------- Compile ---------------- */

/* lua_dump writer: grows a heap buffer. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} dump_buf_t;

static int dump_to_buf(lua_State *L, const void *p, size_t size, void *ud) {
    (void)L;
    dump_buf_t *b = (dump_buf_t *)ud;
    if (b->len + size > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 1024;
        while (ncap < b->len + size) {
            ncap *= 2;
        }
        char *nd = (char *)realloc(b->data, ncap);
        if (!nd) {
            return 1;
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, p, size);
    b->len += size;
    return 0;
}

/* Compile(src [, dst]) -> true, bytes | nil, err
 *
 * The same output as the IDE's build and the simulator's --compile: the
 * source is parsed by the OS's own Lua in a scratch state (its parser
 * memory comes from the system heap, not the caller's capped one) and
 * dumped as a binary chunk with debug info kept, named "@<basename>"
 * so runtime errors read like the source's. `dst` defaults to `src`
 * with its .lua suffix replaced by .prg (or .prg appended). Paths are
 * program-relative like the fs module's. */
static int sys_compile(lua_State *L) {
    (void)program_of(L);
    char src[FS_LUA_PATH_MAX];
    char dst[FS_LUA_PATH_MAX];
    fs_lua_resolve_path(L, luaL_checkstring(L, 1), src);
    if (lua_isnoneornil(L, 2)) {
        size_t n = strlen(src);
        const char *stem = src;
        size_t stem_len = n;
        if (n > 4 && strcasecmp(src + n - 4, ".lua") == 0) {
            stem_len = n - 4;
        }
        if (stem_len + 4 >= sizeof(dst)) {
            lua_pushnil(L);
            lua_pushliteral(L, "path too long");
            return 2;
        }
        snprintf(dst, sizeof(dst), "%.*s.prg", (int)stem_len, stem);
    } else {
        fs_lua_resolve_path(L, luaL_checkstring(L, 2), dst);
    }

    char *source = NULL;
    size_t source_len = 0;
    FRESULT fr = fs_lua_readall(src, &source, &source_len);
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open '%s': %s", src, FRESULT_str(fr));
        return 2;
    }

    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char chunk_name[FS_LUA_PATH_MAX + 1];
    snprintf(chunk_name, sizeof(chunk_name), "@%s", base);

    lua_State *C = luaL_newstate();
    if (!C) {
        free(source);
        lua_pushnil(L);
        lua_pushliteral(L, "out of memory");
        return 2;
    }
    if (luaL_loadbufferx(C, source, source_len, chunk_name, "t") != LUA_OK) {
        const char *message = lua_tostring(C, -1);
        lua_pushnil(L);
        lua_pushstring(L, message ? message : "compile failed");
        lua_close(C);
        free(source);
        return 2;
    }
    free(source);

    dump_buf_t buf = {0};
    int dumped = lua_dump(C, dump_to_buf, &buf, 0);
    lua_close(C);
    if (dumped != 0) {
        free(buf.data);
        lua_pushnil(L);
        lua_pushliteral(L, "out of memory");
        return 2;
    }

    fr = fs_lua_writeall(dst, buf.data, buf.len);
    free(buf.data);
    if (fr != FR_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot write '%s': %s", dst, FRESULT_str(fr));
        return 2;
    }
    lua_pushboolean(L, true);
    lua_pushinteger(L, (lua_Integer)buf.len);
    return 2;
}

static int sys_utility_result(lua_State *L) {
    program_t *p = program_of(L);
    if (p->interactive) {
        return luaL_error(L, "UtilityResult requires a noninteractive program");
    }
    p->utility_result_set = true;
    p->utility_ok = lua_toboolean(L, 1) != 0;
    const char *message = luaL_optstring(L, 2, "");
    snprintf(p->utility_output, sizeof(p->utility_output), "%s", message);
    p->exit_requested = true;
    return 0;
}

static int sys_utility_poll(lua_State *L) {
    program_t *p = program_of(L);
    if (!p->child_result_pending) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, p->child_result_ok);
    lua_pushstring(L, p->child_result_output);
    p->child_result_pending = false;
    p->child_result_output[0] = '\0';
    return 2;
}

/* ---------------- registration ---------------- */

static const luaL_Reg sys_funcs[] = {
    {"TimeNow", sys_timenow},
    {"Pid", sys_pid},
    {"ExitProgram", sys_exit},
    {"Launch", sys_launch},
    {"Execute", sys_execute},
    {"ExecuteString", sys_execute_string},
    {"UtilityResult", sys_utility_result},
    {"UtilityPoll", sys_utility_poll},
    {"TimerCreate", sys_timer_create},
    {"TimerStop", sys_timer_stop},
    {"InputPoll", sys_input_poll},
    {"InputControl", sys_input_control},
    {"WaitVSync", sys_wait_vsync},
    {"Compile", sys_compile},
    {NULL, NULL},
};

void sys_lua_openlibs(lua_State *L) {
    lua_pushglobaltable(L);
    luaL_setfuncs(L, sys_funcs, 0);
    lua_pop(L, 1);
}

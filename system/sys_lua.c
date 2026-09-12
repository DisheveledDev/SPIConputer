/* sys_lua.c
 *
 * OS services exposed to programs (Phase 5). All functions look up
 * their program via the "_spi_program" registry back-reference, so
 * they only work inside a program state.
 *
 *   TimeNow()                     -> milliseconds since boot
 *   Pid()                         -> this program's pid
 *   ExitProgram()                 -> request exit after the current tick
 *   Launch(path [, arg])          -> true | nil, err (pauses the caller)
 *   Execute(path, args...)        -> true | nil, err (foreground run)
 *   ExecuteString(src, args...)   -> true | nil, err (run Lua source)
 *   TimerCreate(fn, ms [, oneshot]) -> timer id
 *   TimerStop(id)                 -> bool
 *   InputPoll()                   -> next event table | nil
 *   InputControl(n)               -> {up,down,left,right,fire} | nil
 */
#include "sys_lua.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lauxlib.h"

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
    const char *err = NULL;
    if (program_launch(path, arg, &err) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "launch failed");
        return 2;
    }
    /* Success: the caller is now paused. */
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
    {NULL, NULL},
};

void sys_lua_openlibs(lua_State *L) {
    lua_pushglobaltable(L);
    luaL_setfuncs(L, sys_funcs, 0);
    lua_pop(L, 1);
}

/* sys_lua.c
 *
 * OS services exposed to programs (Phase 5). All functions look up
 * their program via the "_spi_program" registry back-reference, so
 * they only work inside a program state.
 *
 *   TimeNow()                     -> milliseconds since boot
 *   Pid()                         -> this program's pid
 *   ExitProgram()                 -> request exit after the current tick
 *   Launch(path)                  -> true | nil, err (pauses the caller)
 *   TimerCreate(fn, ms [, oneshot]) -> timer id
 *   TimerStop(id)                 -> bool
 *   InputPoll()                   -> next event table | nil
 *   InputControl(n)               -> {up,down,left,right,fire} | nil
 */
#include "sys_lua.h"

#include <stdint.h>
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

/* ---------------- registration ---------------- */

static const luaL_Reg sys_funcs[] = {
    {"TimeNow", sys_timenow},
    {"Pid", sys_pid},
    {"ExitProgram", sys_exit},
    {"Launch", sys_launch},
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

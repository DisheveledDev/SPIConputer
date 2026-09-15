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
        video_frame_wait_hook();
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

/* ---------------- utility results ---------------- */

/* A utility's result crosses Lua states, so a table is serialised to
 * Lua source ("{[\"n\"]=1,...}") in the utility's state and loaded again
 * in the caller's. Strings, numbers, booleans and nested tables with
 * string or integer keys are kept; anything else (functions, userdata)
 * becomes nil. Bounded in size and depth. */
#define UTILITY_RESULT_MAX 8192
#define UTILITY_DEPTH_MAX 8

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} dyn_buf_t;

static void dyn_append(dyn_buf_t *b, const char *s, size_t n) {
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        while (ncap < b->len + n + 1) ncap *= 2;
        if (ncap > UTILITY_RESULT_MAX + 256) {
            b->failed = true;
            return;
        }
        char *nd = (char *)realloc(b->data, ncap);
        if (!nd) {
            b->failed = true;
            return;
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void dyn_puts(dyn_buf_t *b, const char *s) {
    dyn_append(b, s, strlen(s));
}

static void dyn_quote(dyn_buf_t *b, const char *s, size_t n) {
    dyn_puts(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char tmp[8];
        if (c == '"' || c == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)c;
            dyn_append(b, tmp, 2);
        } else if (c == '\n') {
            dyn_puts(b, "\\n");
        } else if (c < 32 || c == 127) {
            snprintf(tmp, sizeof(tmp), "\\%03u", c);
            dyn_puts(b, tmp);
        } else {
            dyn_append(b, (const char *)&s[i], 1);
        }
    }
    dyn_puts(b, "\"");
}

static void serialize_value(lua_State *L, int idx, dyn_buf_t *b, int depth) {
    idx = lua_absindex(L, idx);
    char tmp[64];
    switch (lua_type(L, idx)) {
        case LUA_TBOOLEAN:
            dyn_puts(b, lua_toboolean(L, idx) ? "true" : "false");
            return;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                snprintf(tmp, sizeof(tmp), LUA_INTEGER_FMT, lua_tointeger(L, idx));
            } else {
                snprintf(tmp, sizeof(tmp), "%.17g", (double)lua_tonumber(L, idx));
            }
            dyn_puts(b, tmp);
            return;
        case LUA_TSTRING: {
            size_t n;
            const char *s = lua_tolstring(L, idx, &n);
            dyn_quote(b, s, n);
            return;
        }
        case LUA_TTABLE: {
            if (depth >= UTILITY_DEPTH_MAX) {
                dyn_puts(b, "nil");
                return;
            }
            dyn_puts(b, "{");
            bool first = true;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                int key = lua_absindex(L, -2);
                int value = lua_absindex(L, -1);
                bool keyed = false;
                if (lua_isinteger(L, key)) {
                    snprintf(tmp, sizeof(tmp), "[" LUA_INTEGER_FMT "]=",
                             lua_tointeger(L, key));
                    if (!first) dyn_puts(b, ",");
                    dyn_puts(b, tmp);
                    keyed = true;
                } else if (lua_type(L, key) == LUA_TSTRING) {
                    size_t n;
                    const char *s = lua_tolstring(L, key, &n);
                    if (!first) dyn_puts(b, ",");
                    dyn_puts(b, "[");
                    dyn_quote(b, s, n);
                    dyn_puts(b, "]=");
                    keyed = true;
                }
                if (keyed) {
                    serialize_value(L, value, b, depth + 1);
                    first = false;
                }
                lua_pop(L, 1);
            }
            dyn_puts(b, "}");
            return;
        }
        default:
            dyn_puts(b, "nil");
            return;
    }
}

/* UtilityResult(ok [, value]): value is a string (default "") or a
 * table; the utility exits after the current callback. */
static int sys_utility_result(lua_State *L) {
    program_t *p = program_of(L);
    /* Any program may end with a result: a utility always does, and an
     * interactive program (a picker, a dialog) uses it to hand a choice
     * back to the program that launched it. */
    dyn_buf_t b = {0};
    bool is_table = lua_type(L, 2) == LUA_TTABLE;
    if (is_table) {
        serialize_value(L, 2, &b, 0);
    } else {
        const char *message = luaL_optstring(L, 2, "");
        dyn_puts(&b, message);
    }
    if (b.failed || !b.data) {
        free(b.data);
        return luaL_error(L, "utility result too large (max %d bytes)",
                          UTILITY_RESULT_MAX);
    }
    free(p->utility_output);
    p->utility_output = b.data;
    p->utility_is_table = is_table;
    p->utility_result_set = true;
    p->utility_ok = lua_toboolean(L, 1) != 0;
    p->exit_requested = true;
    return 0;
}

/* Rebuild a serialized result (the restricted table-constructor text
 * serialize_value writes) directly on the Lua stack. Parsing it as Lua
 * source instead compiled a function holding every string as a constant
 * in the parent's heap, several times the result's size: a resident
 * shell near its cap ran out of memory on an 8 KB listing. This reader
 * allocates only the resulting tables and strings. */
typedef struct {
    const char *p;
    const char *end;
} result_reader_t;

static bool read_value(lua_State *L, result_reader_t *r, int depth);

static bool read_string(lua_State *L, result_reader_t *r) {
    if (r->p >= r->end || *r->p != '"') return false;
    r->p++;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    while (r->p < r->end && *r->p != '"') {
        char c = *r->p++;
        if (c == '\\' && r->p < r->end) {
            char e = *r->p++;
            if (e == 'n') {
                c = '\n';
            } else if (e >= '0' && e <= '9') {
                int v = e - '0';
                for (int i = 0; i < 2 && r->p < r->end && *r->p >= '0' && *r->p <= '9'; i++) {
                    v = v * 10 + (*r->p++ - '0');
                }
                c = (char)v;
            } else {
                c = e; /* \" and \\ */
            }
        }
        luaL_addchar(&b, c);
    }
    if (r->p >= r->end) return false;
    r->p++; /* closing quote */
    luaL_pushresult(&b);
    return true;
}

static bool read_value(lua_State *L, result_reader_t *r, int depth) {
    if (r->p >= r->end) return false;
    char c = *r->p;
    if (c == '"') return read_string(L, r);
    if (c == '{') {
        if (depth > UTILITY_DEPTH_MAX || !lua_checkstack(L, 4)) return false;
        r->p++;
        lua_newtable(L);
        while (r->p < r->end && *r->p != '}') {
            if (*r->p == ',') {
                r->p++;
                continue;
            }
            if (*r->p != '[') return false;
            r->p++;
            if (!read_value(L, r, depth + 1)) return false; /* key */
            if (r->p + 1 >= r->end || r->p[0] != ']' || r->p[1] != '=') return false;
            r->p += 2;
            if (!read_value(L, r, depth + 1)) return false; /* value */
            lua_rawset(L, -3);
        }
        if (r->p >= r->end) return false;
        r->p++;
        return true;
    }
    if (r->end - r->p >= 4 && memcmp(r->p, "true", 4) == 0) {
        r->p += 4;
        lua_pushboolean(L, 1);
        return true;
    }
    if (r->end - r->p >= 5 && memcmp(r->p, "false", 5) == 0) {
        r->p += 5;
        lua_pushboolean(L, 0);
        return true;
    }
    if (r->end - r->p >= 3 && memcmp(r->p, "nil", 3) == 0) {
        r->p += 3;
        lua_pushnil(L);
        return true;
    }
    /* A number: integer or %.17g float. */
    char tmp[48];
    size_t n = 0;
    while (r->p < r->end && n < sizeof(tmp) - 1 && strchr("+-0123456789.eEinfa", *r->p)) {
        tmp[n++] = *r->p++;
    }
    tmp[n] = '\0';
    return n > 0 && lua_stringtonumber(L, tmp) != 0;
}

/* UtilityPoll() -> ok, value | nil: the child's result, once. A table
 * result is rebuilt in this state. */
static int sys_utility_poll(lua_State *L) {
    program_t *p = program_of(L);
    if (!p->child_result_pending) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, p->child_result_ok);
    const char *output = p->child_result_output ? p->child_result_output : "";
    bool pushed = false;
    if (p->child_result_is_table) {
        int top = lua_gettop(L);
        result_reader_t r = {output, output + strlen(output)};
        if (read_value(L, &r, 0) && lua_istable(L, -1)) {
            pushed = true;
        } else {
            lua_settop(L, top);
        }
    }
    if (!pushed) {
        lua_pushstring(L, output);
    }
    free(p->child_result_output);
    p->child_result_output = NULL;
    p->child_result_is_table = false;
    p->child_result_pending = false;
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

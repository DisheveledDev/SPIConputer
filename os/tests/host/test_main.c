/* test_main.c
 *
 * Host-side tests for the Lua <-> filesystem bridge (fs_lua.c) running
 * over the RPC path: the real Lua 5.5 core and the real RPC/fs stack
 * (rpc.c + fs_core0.c + fs_lua.c) against the mock FatFs layer.
 * The RPC wait hook runs the core 0 service inline, exactly as the
 * firmware main loop would.
 *
 * Usage: spicomputer_host_tests [path/to/os.lua]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "f_util.h"
#include "hw_config.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "fs_lua.h"
#include "fs_core0.h"
#include "rpc.h"
#include "system_state.h"
#include "program.h"

extern void mock_set_os_lua(const char *content, size_t len);
extern const char *mock_boot_log(void);
extern void mock_sd_eject(void);
extern void mock_sd_insert(void);

static int g_failures = 0;

/* RPC wait hook: run the core 0 service synchronously (single-threaded
 * stand-in for the firmware main loop). */
static void rpc_wait_host(void) {
    fs_core0_service();
}

static void run_test(int n, const char *name, lua_State *L, const char *code) {
    printf("=== test %d: %s ===\n", n, name);
    if (luaL_dostring(L, code) != LUA_OK) {
        printf("TEST FAIL: %s: %s\n", name, lua_tostring(L, -1));
        g_failures++;
    }
}

int main(int argc, char **argv) {
    const char *script_path = argc > 1 ? argv[1] : "../../os.lua";
    FILE *f = fopen(script_path, "rb");
    if (!f) {
        printf("cannot open boot script '%s'\n", script_path);
        return 1;
    }
    static char script[8192];
    size_t slen = fread(script, 1, sizeof(script) - 1, f);
    fclose(f);
    script[slen] = 0;
    mock_set_os_lua(script, slen);

    /* RPC transport: core 0 side runs inline in the wait hook. */
    rpc_bind_wait(rpc_wait_host);
    rpc_bind_signal(NULL);

    lua_State *L = luaL_newstate();
    fs_lua_openlibs(L);

    /* SD mount happens on core 0 (Phase 4). */
    if (!fs_core0_mount()) { printf("TEST FAIL: mount\n"); return 1; }

    /* 1. Boot the shell program through the process model */
    printf("=== test 1: program_boot(os.lua) ===\n");
    if (!program_boot("os.lua", NULL)) {
        printf("TEST FAIL: program boot\n");
        g_failures++;
    } else {
        program_t *shell = program_top();
        if (!shell || shell->pid != 0) {
            printf("TEST FAIL: shell is not pid 0\n");
            g_failures++;
        }
        if (g_current_video != shell->video) {
            printf("TEST FAIL: video state not current\n");
            g_failures++;
        }
        /* One scheduler step (setup already ran during boot). */
        program_scheduler_step();
        /* Quit the shell cleanly: finish() runs. */
        program_terminate(shell);
        if (program_top() != NULL || g_current_video != NULL) {
            printf("TEST FAIL: stack not empty after shell exit\n");
            g_failures++;
        }
        if (strstr(mock_boot_log(), "shell-finish\n") == NULL) {
            printf("TEST FAIL: shell finish not recorded\n");
            g_failures++;
        }
    }

    /* 2. dofile from within Lua (nested) */
    run_test(2, "dofile('os.lua') from Lua", L, "dofile('os.lua')");

    /* 3. loadfile semantics */
    run_test(3, "loadfile", L,
            "local f, err = loadfile('os.lua')\n"
            "assert(type(f) == 'function', 'not a function: ' .. tostring(err))\n"
            "local ok, e = pcall(f)\n"
            "assert(ok, e)");

    /* 4. require() from SD via custom searcher */
    run_test(4, "require('lib.hello')", L,
            "local v = require('lib.hello')\n"
            "assert(v == 'hello-ok', 'bad return: ' .. tostring(v))");

    /* 5. fs module API */
    run_test(5, "fs module", L,
            "local fs = require('fs')\n"
            "assert(fs.ready() == true)\n"
            "local fk, tk = fs.free()\n"
            "assert(fk == 3200 and tk == 63936, ('free %d %d'):format(fk, tk))\n"
            "local st = fs.stat('os.lua')\n"
            "assert(st and st.size > 0)\n"
            "assert(fs.exists('os.lua') and not fs.exists('nope.lua'))\n"
            "local names = {}\n"
            "for _, e in ipairs(fs.ls('/')) do names[e.name] = true end\n"
            "assert(names['os.lua'] and names['lib'])\n"
            "local wf = assert(fs.open('boot.log', 'w'))\n"
            "assert(wf:write('first\\n') == 6)\n"
            "wf:close()\n"
            "local fh = assert(fs.open('boot.log', 'a'))\n"
            "assert(fh:write('more\\n') == 5)\n"
            "assert(fh:flush() == true)\n"
            "assert(fh:close() == true)\n"
            "local rf = assert(fs.open('boot.log', 'r'))\n"
            "local s = rf:read(100)\n"
            "assert(s == 'first\\nmore\\n', 'got: ' .. tostring(s))\n"
            "assert(rf:seek(0, 'set') == 0)\n"
            "assert(rf:size() == #s)\n"
            "rf:close()\n");

    /* 6. error handling: missing files */
    run_test(6, "missing file errors", L,
            "local ok = pcall(dofile, 'missing.lua')\n"
            "assert(not ok)\n"
            "local f, err = loadfile('missing.lua')\n"
            "assert(f == nil and err ~= nil)\n");

    /* 7. fs.readall / fs.writeall over the RPC path */
    run_test(7, "fs.readall/writeall", L,
            "local fs = require('fs')\n"
            "local s = fs.readall('os.lua')\n"
            "assert(s and #s > 0)\n"
            "assert(fs.writeall('boot.log', 'readall-writeall\\n') == true)\n"
            "assert(fs.readall('boot.log') == 'readall-writeall\\n')\n");

    /* 8. SD card ejected: clean Lua errors, not crashes */
    mock_sd_eject();
    run_test(8, "ejected SD card", L,
            "local fs = require('fs')\n"
            "local f, err = fs.open('os.lua', 'r')\n"
            "assert(f == nil and err ~= nil, 'expected nil+err')\n"
            "local ok = pcall(dofile, 'os.lua')\n"
            "assert(not ok)\n");
    mock_sd_insert();

    /* 9. Explicit RPC round-trip against the mock core 0 service */
    printf("=== test 9: RPC round-trip ===\n");
    {
        rpc_request_t req;
        rpc_response_t resp;
        memset(&req, 0, sizeof(req));
        req.op = RPC_FS_OPEN;
        req.a = FA_CREATE_ALWAYS | FA_WRITE;
        snprintf(req.path1, sizeof(req.path1), "boot.log");
        rpc_call(&req, &resp);
        if (resp.result != FR_OK || resp.value <= 0) {
            printf("TEST FAIL: RPC open\n");
            g_failures++;
        } else {
            int32_t handle = resp.value;
            const char *msg = "rpc-ok";
            memcpy(rpc_staging(), msg, 6);
            memset(&req, 0, sizeof(req));
            req.op = RPC_FS_WRITE;
            req.a = handle;
            req.b = 6;
            rpc_call(&req, &resp);
            if (resp.result != FR_OK || resp.value != 6) {
                printf("TEST FAIL: RPC write\n");
                g_failures++;
            }
            memset(&req, 0, sizeof(req));
            req.op = RPC_FS_READ;
            req.a = handle;
            req.b = 6;
            rpc_call(&req, &resp);
            if (resp.result != FR_OK || resp.value != 6 ||
                memcmp(rpc_staging(), msg, 6) != 0) {
                printf("TEST FAIL: RPC read\n");
                g_failures++;
            }
            memset(&req, 0, sizeof(req));
            req.op = RPC_FS_CLOSE;
            req.a = handle;
            rpc_call(&req, &resp);
            if (resp.result != FR_OK) {
                printf("TEST FAIL: RPC close\n");
                g_failures++;
            }
        }
    }

    lua_close(L);
    if (g_failures == 0) {
        printf("\nALL TESTS PASSED\n");
    } else {
        printf("\n%d TEST(S) FAILED\n", g_failures);
    }
    return g_failures ? 1 : 0;
}

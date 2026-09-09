/* test_main.c
 *
 * Host-side tests for the Lua <-> FatFs bridge (fatfs_lua.c).
 * Runs against the mock FatFs layer (mock/) and the real Lua 5.5 core.
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
#include "fatfs_lua.h"

extern void mock_set_os_lua(const char *content, size_t len);
extern const char *mock_boot_log(void);

static int g_failures = 0;

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

    lua_State *L = luaL_newstate();
    fatfs_lua_openlibs(L);

    if (!fatfs_lua_mount()) { printf("TEST FAIL: mount\n"); return 1; }

    /* 1. Run os.lua through the SD-backed loader path */
    printf("=== test 1: fatfs_lua_run_file(os.lua) ===\n");
    if (fatfs_lua_run_file(L, "os.lua") != 0) {
        printf("TEST FAIL: run os.lua\n");
        g_failures++;
    }
    if (strcmp(mock_boot_log(), "boot ok\n") != 0) {
        printf("TEST FAIL: boot.log = '%s'\n", mock_boot_log());
        g_failures++;
    } else {
        printf("boot.log write OK\n");
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

    lua_close(L);
    if (g_failures == 0) {
        printf("\nALL TESTS PASSED\n");
    } else {
        printf("\n%d TEST(S) FAILED\n", g_failures);
    }
    return g_failures ? 1 : 0;
}
